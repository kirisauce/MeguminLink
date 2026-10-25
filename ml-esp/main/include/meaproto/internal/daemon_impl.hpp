#include "meaproto/daemon.hpp"
#include "meguminlink/wireless/espnow_transport.hpp"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>

namespace meaproto
{
namespace
{
constexpr char TAG[] = "meaproto::daemon";

/** 统计日志间隔 */
constexpr u32 STATS_LOG_INTERVAL_MS = 10000;

/** stop() 等待任务退出的上限 */
constexpr u32 STOP_TIMEOUT_MS = 1000;

/** 通道存储：内部 RAM，8 位可寻址 */
u8 *allocate_storage(usize bytes) noexcept
{
    if (bytes == 0)
    {
        return nullptr;
    }

    u8 *storage = static_cast<u8 *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (storage == nullptr)
    {
        ESP_LOGE(TAG, "failed to allocate %u bytes of channel storage", static_cast<unsigned>(bytes));
    }
    return storage;
}

u32 now_ms() noexcept
{
    return static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/** u32 计时回绕安全：now 是否已到 deadline */
bool time_after(u32 now, u32 deadline) noexcept
{
    return static_cast<i32>(now - deadline) >= 0;
}

const char *state_name(Core::State state) noexcept
{
    switch (state)
    {
    case Core::State::Idle:
        return "idle";
    case Core::State::Connecting:
        return "connecting";
    case Core::State::Connected:
        return "connected";
    }
    return "unknown";
}
} // namespace

template <Transport T>
Daemon<T>::Daemon(T &transport, Sink &sink, std::span<const ChannelSpec> channels, const Config &config) noexcept
    : m_transport(transport), m_storage_bytes(Core::storage_bytes(channels, config, static_cast<u16>(T::MTU))),
      m_storage(allocate_storage(m_storage_bytes)),
      m_core(channels, config, static_cast<u16>(T::MTU), sink, CoreHooks{this, &Daemon<T>::send_hook, &random_epoch},
             std::span<u8>{m_storage, m_storage_bytes}),
      m_rx_depth(config.rx_queue_depth == 0 ? 1 : config.rx_queue_depth),
      m_rx_item(static_cast<u16>(sizeof(u16) + T::MTU))
{
    m_core_mutex = xSemaphoreCreateMutex();
    m_rx_queue = xQueueCreate(m_rx_depth, m_rx_item);
    m_stopped = xSemaphoreCreateBinary();
    if (m_core_mutex == nullptr || m_rx_queue == nullptr || m_stopped == nullptr)
    {
        ESP_LOGE(TAG, "failed to create core mutex, rx queue or stop semaphore");
    }
    m_transport.set_receive_handler(&Daemon<T>::receive_hook, this);
}

template <Transport T> Daemon<T>::~Daemon()
{
    stop();

    heap_caps_free(m_storage);
    m_storage = nullptr;

    if (m_rx_queue != nullptr)
    {
        vQueueDelete(m_rx_queue);
        m_rx_queue = nullptr;
    }
    if (m_stopped != nullptr)
    {
        vSemaphoreDelete(m_stopped);
        m_stopped = nullptr;
    }
    if (m_core_mutex != nullptr)
    {
        vSemaphoreDelete(m_core_mutex);
        m_core_mutex = nullptr;
    }
}

template <Transport T> void Daemon<T>::start(UBaseType_t priority, usize stack)
{
    if (m_rx_queue == nullptr || m_stopped == nullptr || m_task.load(std::memory_order_relaxed) != nullptr)
    {
        return;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreate(&Daemon<T>::task_entry, "meaproto", static_cast<configSTACK_DEPTH_TYPE>(stack), this, priority,
                    &task) != pdPASS)
    {
        ESP_LOGE(TAG, "failed to create task");
        return;
    }
    m_task.store(task, std::memory_order_relaxed);
}

template <Transport T> void Daemon<T>::stop()
{
    // 先摘句柄：之后的 open()/close()/收帧只会看到空句柄，不会再通知这条要退出的任务
    const TaskHandle_t task = m_task.exchange(nullptr, std::memory_order_acq_rel);
    if (task == nullptr)
    {
        return;
    }

    m_stopping.store(true, std::memory_order_relaxed);
    xTaskNotifyGive(task);
    (void)xSemaphoreTake(m_stopped, pdMS_TO_TICKS(STOP_TIMEOUT_MS));
    m_stopping.store(false, std::memory_order_relaxed);
}

template <Transport T> void Daemon<T>::open()
{
    m_request.store(Request::Open, std::memory_order_relaxed);
    wake();
}

template <Transport T> void Daemon<T>::close()
{
    m_request.store(Request::Close, std::memory_order_relaxed);
    wake();
}

template <Transport T> bool Daemon<T>::connected() const
{
    return m_state.load(std::memory_order_relaxed) == Core::State::Connected;
}

template <Transport T> bool Daemon<T>::connecting() const
{
    return m_state.load(std::memory_order_relaxed) == Core::State::Connecting;
}

template <Transport T> Stats Daemon<T>::snapshot() const
{
    taskENTER_CRITICAL(&m_snapshot_lock);
    const Stats value = m_snapshot;
    taskEXIT_CRITICAL(&m_snapshot_lock);
    return value;
}

template <Transport T> const Config &Daemon<T>::config() const
{
    return m_core.config();
}

template <Transport T> SendResult Daemon<T>::send(ChannelId channel, std::span<const u8> data)
{
    if (m_task.load(std::memory_order_relaxed) == nullptr)
    {
        return SendResult::InvalidState;
    }

    lock_core();
    const SendResult result = m_core.send(channel, data);
    unlock_core();

    if (result == SendResult::Ok)
    {
        wake(); // 入队后立刻唤醒任务，否则要等下一次保活或重传节拍
    }
    return result;
}

template <Transport T> bool Daemon<T>::can_send(ChannelId channel) const
{
    if (m_task.load(std::memory_order_relaxed) == nullptr)
    {
        return false;
    }

    lock_core();
    const bool space = m_core.can_send(channel);
    unlock_core();
    return space;
}

template <Transport T> bool Daemon<T>::set_reliability(ChannelId channel, Reliability reliability)
{
    if (m_task.load(std::memory_order_relaxed) == nullptr)
    {
        return false;
    }

    lock_core();
    const bool changed = m_core.set_reliability(channel, reliability);
    unlock_core();

    if (changed)
    {
        wake();
    }
    return changed;
}

template <Transport T> bool Daemon<T>::ping()
{
    if (m_task.load(std::memory_order_relaxed) == nullptr)
    {
        return false;
    }

    lock_core();
    const bool sent = m_core.ping(now_ms());
    unlock_core();

    if (sent)
    {
        wake();
    }
    return sent;
}

template <Transport T> void Daemon<T>::task_entry(void *ctx)
{
    static_cast<Daemon<T> *>(ctx)->run();
}

template <Transport T> bool Daemon<T>::send_hook(void *ctx, std::span<const u8> data) noexcept
{
    return static_cast<Daemon<T> *>(ctx)->m_transport.send(data);
}

template <Transport T> void Daemon<T>::receive_hook(void *ctx, const u8 *data, usize len) noexcept
{
    static_cast<Daemon<T> *>(ctx)->push_frame(data, len);
}

template <Transport T> void Daemon<T>::push_frame(const u8 *data, usize len)
{
    // m_rx_staging 只被 Transport 的接收回调写；约定该回调串行
    if (m_rx_queue == nullptr || len > static_cast<usize>(m_rx_item) - sizeof(u16))
    {
        m_rx_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const u16 size = static_cast<u16>(len);
    std::memcpy(m_rx_staging, &size, sizeof(size));
    std::memcpy(m_rx_staging + sizeof(size), data, len);
    if (xQueueSend(m_rx_queue, m_rx_staging, 0) != pdTRUE)
    {
        m_rx_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    wake();
}

template <Transport T> void Daemon<T>::publish()
{
    const Core::State state = m_core.state();
    m_state.store(state, std::memory_order_relaxed);

    Stats stats = m_core.snapshot();
    stats.connected = (state == Core::State::Connected);
    stats.rx_queue_dropped += m_rx_dropped.load(std::memory_order_relaxed);

    taskENTER_CRITICAL(&m_snapshot_lock);
    m_snapshot = stats;
    taskEXIT_CRITICAL(&m_snapshot_lock);

    if (state == m_logged_state)
    {
        return;
    }

    if (state == Core::State::Connected)
    {
        ESP_LOGI(TAG, "connected: version=%u mtu=%u config_hash=0x%04X own_epoch=0x%08X peer_epoch=0x%08X",
                 static_cast<unsigned>(VERSION), static_cast<unsigned>(m_core.mtu()),
                 static_cast<unsigned>(m_core.config_hash()), static_cast<unsigned>(m_core.own_epoch()),
                 static_cast<unsigned>(m_core.peer_epoch()));
    }
    else
    {
        ESP_LOGI(TAG, "state: %s", state_name(state));
    }
    m_logged_state = state;
}

template <Transport T> void Daemon<T>::run()
{
    lock_core();
    publish();
    unlock_core();

    for (;;)
    {
        const u32 now = now_ms();
        lock_core();
        apply_requests(now);
        drain_rx(now);
        const u32 next = m_core.step(now);
        publish();
        unlock_core();
        log_stats(now);

        if (m_stopping.load(std::memory_order_relaxed))
        {
            break;
        }

        const u32 wait = time_after(next, now) ? (next - now) : 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }

    xSemaphoreGive(m_stopped);
    vTaskDelete(nullptr);
}

template <Transport T> void Daemon<T>::apply_requests(u32 now)
{
    switch (m_request.exchange(Request::None, std::memory_order_relaxed))
    {
    case Request::Open:
        m_core.open(now);
        break;
    case Request::Close:
        m_core.close(now);
        break;
    case Request::None:
        break;
    }
}

template <Transport T> void Daemon<T>::drain_rx(u32 now)
{
    while (m_rx_queue != nullptr)
    {
        if (xQueueReceive(m_rx_queue, m_rx_frame, 0) != pdTRUE)
        {
            return;
        }

        u16 len = 0;
        std::memcpy(&len, m_rx_frame, sizeof(len));
        if (len > static_cast<u16>(m_rx_item - sizeof(u16)))
        {
            continue;
        }
        m_core.receive(now, std::span<const u8>{m_rx_frame + sizeof(u16), len});
    }
}

template <Transport T> void Daemon<T>::log_stats(u32 now)
{
    if (!time_after(now, m_next_stats_ms))
    {
        return;
    }
    m_next_stats_ms = now + STATS_LOG_INTERVAL_MS;

    const Stats stats = snapshot();
    ESP_LOGI(TAG, "stats: connected=%d tx=%u retx=%u acked=%u inflight=%u dup=%u oow=%u rx_dropped=%u rtt=%u rto=%u",
             stats.connected ? 1 : 0, static_cast<unsigned>(stats.datagrams_sent),
             static_cast<unsigned>(stats.datagrams_retransmitted), static_cast<unsigned>(stats.datagrams_acked),
             static_cast<unsigned>(stats.datagrams_in_flight), static_cast<unsigned>(stats.duplicate_datagrams),
             static_cast<unsigned>(stats.out_of_window), static_cast<unsigned>(stats.rx_queue_dropped),
             static_cast<unsigned>(stats.rtt_ms), static_cast<unsigned>(stats.rto_ms));
}

template <Transport T> void Daemon<T>::lock_core() const
{
    // 互斥量创建失败时退化为无锁：构造时已报过错，这里不重复
    if (m_core_mutex != nullptr)
    {
        (void)xSemaphoreTake(m_core_mutex, portMAX_DELAY);
    }
}

template <Transport T> void Daemon<T>::unlock_core() const
{
    if (m_core_mutex != nullptr)
    {
        (void)xSemaphoreGive(m_core_mutex);
    }
}

template <Transport T> void Daemon<T>::wake()
{
    const TaskHandle_t task = m_task.load(std::memory_order_relaxed);
    if (task != nullptr)
    {
        xTaskNotifyGive(task);
    }
}
} // namespace meaproto
