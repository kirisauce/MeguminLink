#include "meguminlink/wireless/arq_link.hpp"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <sdkconfig.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::arq_link";

namespace
{
// NVS 键名上限 15 字符
constexpr char KEY_ARQ_GENERATION[] = "arq_gen";
constexpr char KEY_ARQ_WINDOW[] = "arq_window";
constexpr char KEY_ARQ_MAX_RETX[] = "arq_max_retx";
constexpr char KEY_ARQ_BREAKER[] = "arq_breaker";
constexpr char KEY_ARQ_RTO_MIN[] = "arq_rto_min";
constexpr char KEY_ARQ_RTO_INIT[] = "arq_rto_init";
constexpr char KEY_ARQ_RTO_MAX[] = "arq_rto_max";
constexpr char KEY_ARQ_DELAYED_ACK[] = "arq_delayed_ack";
constexpr char KEY_ARQ_BREAKER_PROBE[] = "arq_probe";

/**
 * 把窗口收敛到本固件编译期允许的上限（Kconfig 的 ARQ_MAX_WINDOW）。
 *
 * 主机下发的值与对端传来的值都可能是更大窗口那份固件给的；本固件按自己的上限
 * 为发送窗口与乱序缓冲分配静态存储，因此只认自己的上限——收敛后再存，上报与
 * 转发出去的也就是这个值，不会出现"声称支持 8、实际只按 1 跑"。
 */
ArqSettings clamp_window(ArqSettings settings)
{
    const u8 cap = static_cast<u8>(CONFIG_MEGUMINLINK_WIRELESS_ARQ_MAX_WINDOW);
    if (settings.window > cap)
    {
        settings.window = cap;
    }
    return settings;
}
} // namespace

// ---- 参数：默认值、校验、NVS 存取 ----

ArqSettings mlink::arq_default_settings()
{
    ArqSettings out = {};
    out.generation = 0;
    // 窗口 4：无线 DAP 的响应方向是瓶颈（写命令对读响应），窗口 1 时"上一条响应还在
    // 等确认、下一条已经执行完"是常态，响应只能排队等；4 条在途实测把响应吞吐抬起来，
    // 同时远低于编译期上限（arq::MAX_WINDOW = 8），重排缓冲本来就按上限分配
    out.window = 4;
    out.max_retx = 6;
    out.breaker_failures = 3;
    out.rto_min_ms = 60;
    out.initial_rto_ms = 60;
    out.rto_max_ms = 250;
    out.delayed_ack_ms = 2;
    out.breaker_probe_ms = 1000;
    return out;
}

arq::Config mlink::arq_config_from(const ArqSettings &settings)
{
    arq::Config config;
    config.window = settings.window;
    // 窗口大于 1 必须开重排：同时在途的多条消息一旦乱序，只能靠乱序缓冲补齐，
    // 否则整窗判超窗、退化成靠重传逐条补齐
    config.reorder = (settings.window > 1);
    config.rto_min_ms = settings.rto_min_ms;
    config.initial_rto_ms = settings.initial_rto_ms;
    config.rto_max_ms = settings.rto_max_ms;
    config.max_retx = settings.max_retx;
    config.breaker_failures = settings.breaker_failures;
    config.delayed_ack_ms = settings.delayed_ack_ms;
    config.breaker_probe_ms = settings.breaker_probe_ms;
    return config; // 越界值由核心自己收敛
}

bool mlink::arq_settings_valid(const ArqSettings &settings)
{
    return settings.window >= 1 && settings.window <= arq::MAX_WINDOW && settings.max_retx >= 1 &&
           settings.rto_min_ms >= 1 && settings.rto_max_ms >= settings.rto_min_ms &&
           settings.initial_rto_ms >= 1 && settings.delayed_ack_ms >= 1 && settings.breaker_probe_ms >= 1;
}

ArqSettings mlink::arq_settings_load()
{
    ArqSettings out = arq_default_settings();

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return out; // 无记录：用默认值
    }

    u32 u32_value = 0;
    u16 u16_value = 0;
    u8 u8_value = 0;

    if (nvs_get_u32(handle, KEY_ARQ_GENERATION, &u32_value) == ESP_OK)
    {
        out.generation = u32_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_WINDOW, &u8_value) == ESP_OK)
    {
        out.window = u8_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_MAX_RETX, &u8_value) == ESP_OK)
    {
        out.max_retx = u8_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_BREAKER, &u8_value) == ESP_OK)
    {
        out.breaker_failures = u8_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_MIN, &u16_value) == ESP_OK)
    {
        out.rto_min_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_INIT, &u16_value) == ESP_OK)
    {
        out.initial_rto_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_MAX, &u16_value) == ESP_OK)
    {
        out.rto_max_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_DELAYED_ACK, &u16_value) == ESP_OK)
    {
        out.delayed_ack_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_BREAKER_PROBE, &u16_value) == ESP_OK)
    {
        out.breaker_probe_ms = u16_value;
    }

    nvs_close(handle);

    if (!arq_settings_valid(out))
    {
        ESP_LOGW(TAG, "stored ARQ settings invalid, falling back to defaults");
        return arq_default_settings();
    }
    return out;
}

esp_err_t mlink::arq_settings_store(const ArqSettings &settings)
{
    NvsWriteBatch batch(NVS_NAMESPACE);
    if (!batch.ready())
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 整份参数走同一个 commit：NVS 保证要么全生效、要么全不生效
    esp_err_t ret = batch.set_u32(KEY_ARQ_GENERATION, settings.generation);
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_WINDOW, settings.window);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_MAX_RETX, settings.max_retx);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_BREAKER, settings.breaker_failures);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_MIN, settings.rto_min_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_INIT, settings.initial_rto_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_MAX, settings.rto_max_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_DELAYED_ACK, settings.delayed_ack_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_BREAKER_PROBE, settings.breaker_probe_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.commit();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to store ARQ settings: %s", esp_err_to_name(ret));
    }
    return ret;
}

// ---- 构造与配置 ----

esp_err_t ArqLink::init(const ArqSettings &settings, size_t payload_cap)
{
    if (payload_cap == 0 || payload_cap > MAX_PAYLOAD || !arq_settings_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    m_settings = clamp_window(settings);
    m_payload_cap = payload_cap;

    // 本机 MAC：参数平局时用它做确定性判断
    esp_read_mac(m_own_mac, ESP_MAC_WIFI_STA);

    m_wake = xSemaphoreCreateBinary();
    m_state_mutex = xSemaphoreCreateMutex();
    m_submit_mutex = xSemaphoreCreateMutex();
    m_rx_queue = xQueueCreate(RX_DEPTH, sizeof(RxFrame));
    m_tx_reliable = xQueueCreate(TX_RELIABLE_DEPTH, sizeof(TxMessage));
    m_tx_unreliable = xQueueCreate(TX_UNRELIABLE_DEPTH, sizeof(TxMessage));
    if (m_wake == nullptr || m_state_mutex == nullptr || m_submit_mutex == nullptr || m_rx_queue == nullptr ||
        m_tx_reliable == nullptr || m_tx_unreliable == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    reconfigure(m_settings);
    reset_session(0);
    publish_state();

    if (xTaskCreate(task_fn, "arq", TASK_STACK, this, TASK_PRIORITY, &m_task) != pdPASS)
    {
        ESP_LOGE(TAG, "failed to create ARQ task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ArqLink::set_callbacks(const Callbacks &callbacks)
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_callbacks = callbacks;
    xSemaphoreGive(m_state_mutex);
}

void ArqLink::reconfigure(const ArqSettings &settings)
{
    m_settings = clamp_window(settings);
    m_config = arq_config_from(m_settings);

    // 参数变了就必须重建两端状态：窗口、重传预算与退避全都按新参数算。累计计数
    // 一并归零是刻意的——参数变更即一次新的测量起点，做 A/B 对照时正好干净。
    m_sender.init(m_config, m_tx_storage, m_payload_cap, arq::MAX_WINDOW);
    m_receiver.init(m_config);
    m_rx_reorder.init(m_rx_storage, m_payload_cap, arq::MAX_WINDOW);
    m_sender.set_callbacks(this, &ArqLink::core_transmit, &ArqLink::core_event);
    m_receiver.set_callbacks(this, &ArqLink::core_event);
}

void ArqLink::reset_session(u32 now)
{
    m_sender.reset();
    m_receiver.reset(0);
    m_rx_reorder.reset();
    m_established = false;
    m_ack_scheduled = false;
    m_replay_pending = false;
    m_reset_reply_pending = false;
    m_peer_epoch_known = false;
    std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));
    m_failed_head = 0;
    m_failed_count = 0;

    // 待发环里的消息属于上一个会话（序号基准已经换了），一并丢弃；序号预留随之
    // 归零，让调用方拿到的新序号与新会话的核心序号重新对齐。
    xSemaphoreTake(m_submit_mutex, portMAX_DELAY);
    xQueueReset(m_tx_reliable);
    xQueueReset(m_tx_unreliable);
    m_seq_counter = 0;
    xSemaphoreGive(m_submit_mutex);

    // 换一个 epoch：对端据此把"本端重启后的新会话"与"旧会话的迟到帧"分开
    m_epoch = esp_random() | 1u;
    m_next_reset_tx_ms = now;
}

esp_err_t ArqLink::apply_settings(const ArqSettings &settings)
{
    if (!arq_settings_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_apply_settings = clamp_window(settings);
    m_apply_pending = true;
    xSemaphoreGive(m_state_mutex);

    xSemaphoreGive(m_wake);
    return ESP_OK;
}

ArqSettings ArqLink::settings() const
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const ArqSettings out = m_settings_view;
    xSemaphoreGive(m_state_mutex);
    return out;
}

bool ArqLink::take_settings_dirty()
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const bool dirty = m_settings_dirty;
    m_settings_dirty = false;
    xSemaphoreGive(m_state_mutex);
    return dirty;
}

// ---- 会话 ----

void ArqLink::set_peer(const u8 *peer_mac)
{
    if (peer_mac == nullptr)
    {
        return;
    }

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    std::memcpy(m_peer_pending_mac, peer_mac, sizeof(m_peer_pending_mac));
    m_peer_pending = true;
    m_peer_clear_pending = false;
    xSemaphoreGive(m_state_mutex);

    xSemaphoreGive(m_wake);
}

void ArqLink::clear_peer()
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_peer_pending = false;
    m_peer_clear_pending = true;
    xSemaphoreGive(m_state_mutex);

    xSemaphoreGive(m_wake);
}

bool ArqLink::has_peer() const
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const bool known = m_state.peer_known;
    xSemaphoreGive(m_state_mutex);
    return known;
}

bool ArqLink::established() const
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const bool value = m_state.established;
    xSemaphoreGive(m_state_mutex);
    return value;
}

bool ArqLink::can_submit() const
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const State state = m_state;
    xSemaphoreGive(m_state_mutex);

    if (!state.peer_known || !state.established || state.breaker_open)
    {
        return false;
    }
    return state.in_flight < state.window;
}

bool ArqLink::take_failed(u16 *seq_out)
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    if (m_failed_count == 0)
    {
        xSemaphoreGive(m_state_mutex);
        return false;
    }

    if (seq_out != nullptr)
    {
        *seq_out = m_failed_seqs[m_failed_head];
    }
    m_failed_head = static_cast<u8>((m_failed_head + 1) % arq::MAX_WINDOW);
    m_failed_count--;
    xSemaphoreGive(m_state_mutex);
    return true;
}

void ArqLink::request_link_alive()
{
    m_link_alive_pending = true;
    xSemaphoreGive(m_wake);
}

// ---- 提交 ----

ArqLink::Submit ArqLink::enqueue(const u8 *payload, u16 len, const u8 *dest_mac, const TxOptions &options,
                                 u16 cmd_seq, bool is_response, u16 *seq_out)
{
    if (payload == nullptr || dest_mac == nullptr || len == 0 || len > m_payload_cap)
    {
        return Submit::Invalid;
    }

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const State state = m_state;
    xSemaphoreGive(m_state_mutex);

    const bool reliable = (options.reliable == Reliability::Reliable);
    if (reliable)
    {
        // 可靠消息必须发给会话对端；不可靠消息（探测、配对、串口）的目的地由调用
        // 方给，配对阶段本来就还没有会话对端。
        if (!state.peer_known)
        {
            return Submit::NoPeer;
        }
        // 会话没对齐时发出去会被对端当成重复帧丢掉，而它回报的累积确认号又会把
        // 这些帧"确认"掉——消息静默消失。熔断同理：此刻提交只会白等满预算。
        if (!state.established)
        {
            return Submit::NotEstablished;
        }
        if (state.breaker_open)
        {
            return Submit::BreakerOpen;
        }
    }

    TxMessage &msg = m_tx_post;
    std::memset(&msg, 0, sizeof(msg));
    std::memcpy(msg.dest_mac, dest_mac, sizeof(msg.dest_mac));
    msg.len = len;
    msg.cmd_seq = cmd_seq;
    msg.retx_limit = options.retx_limit;
    msg.reliable = reliable;
    msg.is_response = is_response;
    msg.loss_injectable = options.loss_injectable;
    std::memcpy(msg.data, payload, len);

    // 组帧缓冲与序号预留都是共享的（提交可能来自多个任务上下文），用互斥量串
    // 起来；入队本身是异步的，锁内不做等待。
    const QueueHandle_t queue = reliable ? m_tx_reliable : m_tx_unreliable;
    xSemaphoreTake(m_submit_mutex, portMAX_DELAY);
    if (reliable)
    {
        // 序号按提交顺序预留：待发环是 FIFO、任务按序提交，因此"第 n 条提交"就是
        // 核心分配的"第 n 个序号"，调用方可以立刻拿它做账
        msg.seq_hint = m_seq_counter++;
    }
    const BaseType_t queued = xQueueSend(queue, &msg, 0);
    const u16 seq_hint = msg.seq_hint;
    xSemaphoreGive(m_submit_mutex);

    if (queued != pdTRUE)
    {
        // 环满：可靠方向应当由 can_submit 提前挡成背压，走到这里说明提交方没看
        // 背压（或并发提交撞上），计数后交回调用方决定丢弃还是重试。
        m_tx_dropped = m_tx_dropped + 1;
        return Submit::QueueFull;
    }

    if (seq_out != nullptr)
    {
        *seq_out = seq_hint;
    }

    xSemaphoreGive(m_wake);
    return Submit::Ok;
}

ArqLink::Submit ArqLink::submit(const u8 *payload, u16 len, const u8 *dest_mac, const TxOptions &options,
                                u16 *seq_out)
{
    return enqueue(payload, len, dest_mac, options, 0, false, seq_out);
}

ArqLink::Submit ArqLink::submit_response(u16 cmd_seq, const u8 *payload, u16 len, const u8 *dest_mac,
                                         const TxOptions &options)
{
    return enqueue(payload, len, dest_mac, options, cmd_seq, true, nullptr);
}

void ArqLink::on_frame(const u8 *src_mac, const u8 *data, size_t len)
{
    if (src_mac == nullptr || data == nullptr || len == 0 || len > ARQ_FRAME_MAX_SIZE)
    {
        return;
    }

    // ESP-NOW 回调上下文：只搬运，不解析（解析、判重、排序全在任务里）。
    // 回调单线程串行，因此 m_rx_post 只被这里使用，与任务手里的 m_rx_frame 不共享。
    RxFrame &frame = m_rx_post;
    std::memcpy(frame.src_mac, src_mac, sizeof(frame.src_mac));
    frame.len = static_cast<u16>(len);
    std::memcpy(frame.data, data, len);

    if (xQueueSend(m_rx_queue, &frame, 0) != pdTRUE)
    {
        // 接收环满：可靠帧由对端重传补齐，不可靠帧本就不保证送达
        m_rx_dropped = m_rx_dropped + 1;
        return;
    }
    xSemaphoreGive(m_wake);
}

// ---- 任务主体 ----

void ArqLink::task_fn(void *arg)
{
    static_cast<ArqLink *>(arg)->run_loop();
}

void ArqLink::run_loop()
{
    for (;;)
    {
        const u32 now = now_ms();

        drain_rx(now);
        poll_core(now);
        publish_state();

        // 睡到下一个待办时刻；没有待办就兜底醒来刷新对外缓存（调用方的背压判断
        // 依赖它），新提交与新帧会通过信号量立刻唤醒。
        u32 wait_ms = 50;
        const u32 deadline = next_deadline_ms();
        if (deadline != 0)
        {
            const i32 until = static_cast<i32>(deadline - now_ms());
            if (until <= 0)
            {
                wait_ms = 1;
            }
            else if (static_cast<u32>(until) < wait_ms)
            {
                wait_ms = static_cast<u32>(until);
            }
        }
        xSemaphoreTake(m_wake, pdMS_TO_TICKS(wait_ms));
    }
}

void ArqLink::drain_rx(u32 now)
{
    while (xQueueReceive(m_rx_queue, &m_rx_frame, 0) == pdTRUE)
    {
        handle_frame(m_rx_frame.src_mac, m_rx_frame.data, m_rx_frame.len, now);
    }
}

void ArqLink::handle_frame(const u8 *src_mac, const u8 *data, size_t len, u32 now)
{
    if (len < ARQ_HEADER_SIZE)
    {
        return;
    }

    ArqHeader header = {};
    {
        BinDeserializer head(data, len);
        header.seq = head.read_u16();
        header.ack = head.read_u16();
        header.sack = head.read_u16();
        header.window = head.read_u8();
        header.flags = head.read_u8();
    }

    const u8 kind = header.flags & ARQ_FLAG_KIND_MASK;
    if (kind == ARQ_FLAG_KIND_MASK)
    {
        return; // 形态位同置：非法帧
    }
    if (kind != 0 && (header.flags & ARQ_FLAG_MESSAGE_MASK) != 0)
    {
        return; // 形态位与消息位混用：非法帧
    }
    if (kind == 0 && len == ARQ_HEADER_SIZE)
    {
        return; // 数据帧必须带载荷
    }

    // 任何帧都捎带确认，先统一喂给发送窗口
    m_sender.on_ack(header.ack, header.sack, header.window, now);

    if (kind == ARQ_FLAG_ACK_ONLY)
    {
        return; // 纯确认：确认信息已经在上面消费掉了
    }

    const u8 *payload = data + ARQ_HEADER_SIZE;
    const u16 payload_len = static_cast<u16>(len - ARQ_HEADER_SIZE);

    if (kind == ARQ_FLAG_RESET)
    {
        if (payload_len < ARQ_RESET_PAYLOAD_SIZE)
        {
            return;
        }
        BinDeserializer r(payload, ARQ_RESET_PAYLOAD_SIZE);
        const u32 peer_epoch = r.read_u32();
        ArqSettings peer_settings = {};
        peer_settings.generation = r.read_u32();
        peer_settings.window = r.read_u8();
        peer_settings.max_retx = r.read_u8();
        peer_settings.breaker_failures = r.read_u8();
        peer_settings.rto_min_ms = r.read_u16();
        peer_settings.initial_rto_ms = r.read_u16();
        peer_settings.rto_max_ms = r.read_u16();
        peer_settings.delayed_ack_ms = r.read_u16();
        peer_settings.breaker_probe_ms = r.read_u16();
        const bool echo = (r.read_u16() & 0x0001u) != 0;
        handle_reset(peer_epoch, peer_settings, echo, now);
        return;
    }

    if ((header.flags & ARQ_FLAG_RELIABLE) != 0)
    {
        handle_message(header.seq, (header.flags & ARQ_FLAG_GAP_BEFORE) != 0, payload, payload_len, now);
        return;
    }

    // 不可靠消息：不占序号、不产生确认义务，收到就交付
    if (m_callbacks.deliver != nullptr)
    {
        m_callbacks.deliver(m_callbacks.ctx, src_mac, 0, payload, payload_len);
    }
}

void ArqLink::handle_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u32 now)
{
    const arq::RxResult result = m_receiver.on_frame(seq, gap_before, now);
    if (result == arq::RxResult::Delivered)
    {
        // commit 返回本次连续交付的最后一帧：乱序缓冲补齐时区间会不止一帧
        const u16 last = m_receiver.commit(seq, now);
        deliver_range(seq, payload, len, last);
    }
    else if (result == arq::RxResult::Buffered)
    {
        // 判为窗口内的乱序帧：先把载荷收进乱序缓冲，等 commit 的区间把它补出来
        m_rx_reorder.store(seq, payload, len);
    }
    else if (result == arq::RxResult::Duplicate)
    {
        // 对端在重传：记下来，poll 里重放缓存响应（绝不再执行一次）
        m_replay_pending = true;
        m_replay_cmd_seq = seq;
    }

    if (m_receiver.ack_pending() && !m_ack_scheduled)
    {
        m_ack_scheduled = true;
        m_ack_due_ms = now + m_config.delayed_ack_ms;
    }
}

void ArqLink::deliver_range(u16 first, const u8 *first_payload, u16 first_len, u16 last)
{
    const bool peer_known = m_peer_known;
    const u8 *peer_mac = peer_known ? m_peer_mac : nullptr;

    for (u16 seq = first;; ++seq)
    {
        const u8 *payload = first_payload;
        u16 len = first_len;
        if (seq != first)
        {
            payload = m_rx_reorder.get(seq, &len);
            if (payload == nullptr)
            {
                // 区间内没有载荷：对端声明放弃的空洞，跳过不交付
                if (seq == last)
                {
                    break;
                }
                continue;
            }
        }

        if (m_callbacks.deliver != nullptr)
        {
            m_callbacks.deliver(m_callbacks.ctx, peer_mac, seq, payload, len);
        }

        if (seq != first)
        {
            m_rx_reorder.release(seq);
        }
        if (seq == last)
        {
            break;
        }
    }
}

void ArqLink::handle_reset(u32 peer_epoch, const ArqSettings &peer_settings, bool echo, u32 now)
{
    bool reply = false;
    if (!m_peer_epoch_known || m_peer_epoch != peer_epoch)
    {
        // 对端开了一个新会话（上电、链路重建或它刚被下发了新参数）：本端把状态
        // 拉到同一序号基准
        m_peer_epoch = peer_epoch;
        m_peer_epoch_known = true;
        m_sender.reset();
        m_receiver.reset(0);
        m_rx_reorder.reset();
        m_ack_scheduled = false;
        m_replay_pending = false;
        std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));
        m_established = true;
        reply = true;

        // 参数只在对端换了 epoch（即它确实重启过一次会话）时才评估采纳，回显帧
        // 因此不会来回改参数
        if (arq_settings_valid(peer_settings) && should_adopt_settings(peer_settings))
        {
            reconfigure(peer_settings);
            mark_settings_dirty();
            ESP_LOGI(TAG, "adopted peer ARQ settings (generation %u)", static_cast<unsigned>(peer_settings.generation));
        }
        ESP_LOGI(TAG, "peer session epoch 0x%08X accepted, sequence space realigned", peer_epoch);
    }
    else if (!m_established)
    {
        // 同一会话但本端还没对齐：对端仍在重发请求，对齐并回一个
        m_established = true;
        reply = true;
    }
    else if (!echo)
    {
        // 会话已建立却又收到同 epoch 的请求：本端上一次回显没有送达，再回一次
        reply = true;
    }
    // 会话已建立且收到的是回显：对端只是在确认它收到了本端请求，回显到此为止。
    // 少了这一条，两端会把对方的回显当成新的请求，互相回显下去永不停止。

    if (reply)
    {
        if (static_cast<i32>(now - m_reset_reply_due_ms) >= 0)
        {
            emit_reset(true);
            m_reset_reply_due_ms = now + RESET_REPLY_MIN_MS;
            m_reset_reply_pending = false;
        }
        else
        {
            // 刚回过一次：限速到最小间隔再回，避免对端连发请求时回显风暴
            m_reset_reply_pending = true;
        }
    }
}

bool ArqLink::should_adopt_settings(const ArqSettings &peer) const
{
    const i32 diff = static_cast<i32>(peer.generation - m_settings.generation);
    if (diff > 0)
    {
        return true;
    }
    if (diff < 0)
    {
        return false;
    }

    // 代数相同（典型情形是两块板一起上电）：用 MAC 大小做确定性平局。两端比的是
    // 同一个不等式，因此算出的赢家一致——MAC 小的一方那份参数会被两端共同采用，
    // 不会出现"互相采纳、结果仍然不一致"。
    if (!m_peer_known)
    {
        return false;
    }
    return std::memcmp(m_own_mac, m_peer_mac, sizeof(m_own_mac)) > 0;
}

void ArqLink::mark_settings_dirty()
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_settings_dirty = true;
    xSemaphoreGive(m_state_mutex);
}

void ArqLink::poll_core(u32 now)
{
    // 0) 外部的会话变更：对端换了就重新对齐序号基准，同一个对端重复登记是幂等的
    {
        bool apply_peer = false;
        bool clear = false;
        u8 mac[6] = {};
        xSemaphoreTake(m_state_mutex, portMAX_DELAY);
        if (m_peer_pending)
        {
            std::memcpy(mac, m_peer_pending_mac, sizeof(mac));
            m_peer_pending = false;
            apply_peer = true;
        }
        if (m_peer_clear_pending)
        {
            m_peer_clear_pending = false;
            clear = true;
        }
        xSemaphoreGive(m_state_mutex);

        if (clear)
        {
            m_peer_known = false;
            reset_session(now);
        }
        else if (apply_peer)
        {
            const bool changed = !m_peer_known || std::memcmp(m_peer_mac, mac, sizeof(mac)) != 0;
            std::memcpy(m_peer_mac, mac, sizeof(mac));
            m_peer_known = true;
            if (changed)
            {
                reset_session(now);
            }
        }
    }

    // 0b) 主机下发的参数：重建状态并换 epoch，让对端跟着重新对齐
    {
        bool apply = false;
        ArqSettings settings = {};
        xSemaphoreTake(m_state_mutex, portMAX_DELAY);
        if (m_apply_pending)
        {
            settings = m_apply_settings;
            m_apply_pending = false;
            apply = true;
        }
        xSemaphoreGive(m_state_mutex);

        if (apply)
        {
            reconfigure(settings);
            mark_settings_dirty();
            reset_session(now);
            ESP_LOGI(TAG, "ARQ settings applied (generation %u, window %u, retx %u)",
                     static_cast<unsigned>(settings.generation), settings.window, settings.max_retx);
        }
    }

    // 0c) 链路被证明可用（主循环收到 Pong）：清熔断与连续失败计数
    if (m_link_alive_pending)
    {
        m_link_alive_pending = false;
        if (m_sender.notify_link_alive())
        {
            ESP_LOGW(TAG, "ARQ breaker recovered: link is alive again");
        }
    }

    // 1) 会话未对齐：周期性发重置请求，直到收到对端的 epoch
    if (!m_established && m_peer_known && static_cast<i32>(now - m_next_reset_tx_ms) >= 0)
    {
        emit_reset(false);
        m_next_reset_tx_ms = now + RESET_INTERVAL_MS;
    }

    // 1b) 对端请求的回显（限速到最小间隔）
    if (m_reset_reply_pending && static_cast<i32>(now - m_reset_reply_due_ms) >= 0)
    {
        emit_reset(true);
        m_reset_reply_pending = false;
        m_reset_reply_due_ms = now + RESET_REPLY_MIN_MS;
    }

    // 2) 重复命令触发的响应重放
    if (m_replay_pending)
    {
        m_replay_pending = false;
        const size_t slot = m_replay_cmd_seq % arq::MAX_WINDOW;
        if (m_replay_map_valid[slot] && m_sender.resend(m_replay_map[slot], now))
        {
            m_replayed++;
        }
        else
        {
            // 响应已被确认（对端其实收到了）或还没产生（命令仍在执行）：只能等
            m_replay_missed++;
        }
    }

    // 3) RTO 到期与 MAC 失败触发的重传
    m_sender.poll(now);

    // 4) 攒够时间的纯确认
    if (m_ack_scheduled && static_cast<i32>(now - m_ack_due_ms) >= 0)
    {
        emit_ack(m_receiver.ack(), m_receiver.sack_bitmap(), m_receiver.window());
    }

    // 5) 出向：不可靠方向不等窗口，可靠方向按序等窗口
    drain_tx(now);
}

void ArqLink::drain_tx(u32 now)
{
    // 不可靠消息：全部发出。让串口这类流量不必等可靠方向的窗口，否则窗口一堵
    // 串口就跟着卡住，而它本来是尽力而为的。
    while (xQueueReceive(m_tx_unreliable, &m_tx_msg, 0) == pdTRUE)
    {
        const TxMessage &msg = m_tx_msg;
        send_frame(msg.dest_mac, msg.data, msg.len, 0, 0, m_receiver.ack(), m_receiver.sack_bitmap(),
                   m_receiver.window(), false, msg.loss_injectable);
        // 数据帧捎带确认即完成确认义务
        m_receiver.clear_ack_pending();
        m_ack_scheduled = false;
    }

    // 可靠消息：按提交顺序取，窗口满就停在这一条等确认推进——排队而不是丢弃，
    // 这正是"响应方向被窗口堵住"那条老问题的根治办法。
    while (xQueuePeek(m_tx_reliable, &m_tx_msg, 0) == pdTRUE)
    {
        if (!m_peer_known || !m_established || !m_sender.can_submit(now))
        {
            break;
        }
        if (xQueueReceive(m_tx_reliable, &m_tx_msg, 0) != pdTRUE)
        {
            break;
        }

        const TxMessage &msg = m_tx_msg;
        u16 resp_seq = 0;
        const arq::SubmitResult submitted = m_sender.submit(msg.data, msg.len, now, msg.retx_limit, &resp_seq);
        if (submitted != arq::SubmitResult::Ok)
        {
            // can_submit 刚刚放行、同一个上下文里紧接着提交，走到这里只可能是载荷
            // 尺寸与配置不一致（提交时已挡）——记日志并丢弃，不让环卡住
            ESP_LOGW(TAG, "reliable submit rejected (%d), message dropped", static_cast<int>(submitted));
            continue;
        }

        if (msg.is_response)
        {
            // 记下"这条响应回的是哪条命令"：对端重传该命令时据此重放，不必重新执行
            const size_t slot = msg.cmd_seq % arq::MAX_WINDOW;
            m_replay_map[slot] = resp_seq;
            m_replay_map_valid[slot] = true;
        }
    }
}

u32 ArqLink::next_deadline_ms() const
{
    u32 best = m_sender.next_deadline_ms();

    if (!m_established && m_peer_known)
    {
        if (best == 0 || static_cast<i32>(m_next_reset_tx_ms - best) < 0)
        {
            best = m_next_reset_tx_ms;
        }
    }
    if (m_ack_scheduled && (best == 0 || static_cast<i32>(m_ack_due_ms - best) < 0))
    {
        best = m_ack_due_ms;
    }
    if (m_reset_reply_pending && (best == 0 || static_cast<i32>(m_reset_reply_due_ms - best) < 0))
    {
        best = m_reset_reply_due_ms;
    }
    return best;
}

// ---- 出向封帧 ----

void ArqLink::send_frame(const u8 *dest_mac, const u8 *payload, u16 len, u8 flags, u16 seq, u16 ack, u16 sack,
                         u8 window, bool reliable, bool loss_injectable)
{
    if (m_callbacks.send_raw == nullptr || len > ARQ_MAX_PAYLOAD)
    {
        return;
    }

    BinSerializer s(m_frame, ARQ_HEADER_SIZE + len);
    s.write_u16(seq);
    s.write_u16(ack);
    s.write_u16(sack);
    s.write_u8(window);
    s.write_u8(flags);
    if (len > 0 && payload != nullptr)
    {
        s.write_bytes(payload, len);
    }

    m_callbacks.send_raw(m_callbacks.ctx, dest_mac, m_frame, ARQ_HEADER_SIZE + len, reliable, loss_injectable);
}

void ArqLink::emit_message(const u8 *dest_mac, u16 seq, const u8 *payload, u16 len, u8 flags, u16 ack, u16 sack,
                           u8 window, bool loss_injectable)
{
    send_frame(dest_mac, payload, len, flags, seq, ack, sack, window, true, loss_injectable);
}

void ArqLink::emit_ack(u16 ack, u16 sack, u8 window)
{
    if (!m_peer_known)
    {
        return;
    }
    send_frame(m_peer_mac, nullptr, 0, ARQ_FLAG_ACK_ONLY, 0, ack, sack, window, true, false);
    m_receiver.clear_ack_pending();
    m_ack_scheduled = false;
}

void ArqLink::emit_reset(bool echo)
{
    if (!m_peer_known)
    {
        return;
    }

    BinSerializer w(m_reset_payload, sizeof(m_reset_payload));
    w.write_u32(m_epoch);
    w.write_u32(m_settings.generation);
    w.write_u8(m_settings.window);
    w.write_u8(m_settings.max_retx);
    w.write_u8(m_settings.breaker_failures);
    w.write_u16(m_settings.rto_min_ms);
    w.write_u16(m_settings.initial_rto_ms);
    w.write_u16(m_settings.rto_max_ms);
    w.write_u16(m_settings.delayed_ack_ms);
    w.write_u16(m_settings.breaker_probe_ms);
    w.write_u16(echo ? 0x0001u : 0x0000u);

    send_frame(m_peer_mac, m_reset_payload, ARQ_RESET_PAYLOAD_SIZE, ARQ_FLAG_RESET, 0, m_receiver.ack(),
               m_receiver.sack_bitmap(), m_receiver.window(), true, false);
}

void ArqLink::core_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before)
{
    auto *self = static_cast<ArqLink *>(raw);

    u8 flags = ARQ_FLAG_RELIABLE;
    if (retransmit)
    {
        flags |= ARQ_FLAG_RETRANSMIT;
    }
    if (gap_before)
    {
        flags |= ARQ_FLAG_GAP_BEFORE;
    }

    // 可靠消息一律参与注入丢帧：丢了有重传兜底，正好用来验证 ARQ 扛得住差链路
    self->emit_message(self->m_peer_mac, seq, payload, len, flags, self->m_receiver.ack(),
                       self->m_receiver.sack_bitmap(), self->m_receiver.window(), true);

    // 数据帧捎带确认即完成确认义务
    self->m_receiver.clear_ack_pending();
    self->m_ack_scheduled = false;
}

void ArqLink::core_event(void *raw, arq::Event event, u16 seq, u32 now_ms)
{
    auto *self = static_cast<ArqLink *>(raw);
    (void)now_ms;

    // 计数一律从核心快照读，这里只负责把需要人看的事件打到日志
    switch (event)
    {
    case arq::Event::Retransmit: {
        const arq::SenderSnapshot snap = self->m_sender.snapshot();
        ESP_LOGI(TAG, "retransmit seq=%u srtt=%u rttvar=%u rto=%u", seq, snap.srtt_ms, snap.rttvar_ms, snap.rto_ms);
        break;
    }
    case arq::Event::Failed:
        ESP_LOGW(TAG, "message seq=%u dropped after all retries", seq);
        // 上抛给调用方销账：这条消息不会再有响应，在途条目要尽快释放
        self->record_failed(seq);
        break;
    case arq::Event::BreakerTrip:
        ESP_LOGW(TAG, "link breaker tripped: consecutive messages exhausted retries");
        break;
    case arq::Event::BreakerReset:
        ESP_LOGI(TAG, "link breaker reset");
        break;
    default:
        break;
    }
}

void ArqLink::record_failed(u16 seq)
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    if (m_failed_count == arq::MAX_WINDOW)
    {
        // 队列满：挤掉最老的一条。上层没跟上时保最新更有用——旧的那条早已经
        // 被自己的响应超时销账了。
        m_failed_head = static_cast<u8>((m_failed_head + 1) % arq::MAX_WINDOW);
        m_failed_count--;
    }
    const u8 tail = static_cast<u8>((m_failed_head + m_failed_count) % arq::MAX_WINDOW);
    m_failed_seqs[tail] = seq;
    m_failed_count++;
    xSemaphoreGive(m_state_mutex);
}

void ArqLink::publish_state()
{
    const arq::SenderSnapshot tx = m_sender.snapshot();
    const arq::ReceiverSnapshot rx = m_receiver.snapshot();

    Snapshot snapshot = {};
    snapshot.tx = tx.tx_frames;
    snapshot.retx = tx.retx_frames;
    snapshot.acked = tx.acked_frames;
    snapshot.failed = tx.failed_frames;
    snapshot.breaker_trips = tx.breaker_trips;
    snapshot.srtt_ms = tx.srtt_ms;
    snapshot.rto_ms = tx.rto_ms;
    snapshot.in_flight = tx.in_flight;
    snapshot.established = m_established;
    snapshot.breaker_open = tx.breaker_open;
    snapshot.rx_delivered = rx.delivered;
    snapshot.rx_duplicate = rx.duplicate;
    snapshot.rx_out_of_window = rx.out_of_window;
    snapshot.rx_skipped = rx.skipped;
    snapshot.replayed = m_replayed;
    snapshot.replay_missed = m_replay_missed;
    snapshot.tx_dropped = m_tx_dropped;
    snapshot.rx_dropped = m_rx_dropped;

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_state.peer_known = m_peer_known;
    m_state.established = m_established;
    m_state.breaker_open = tx.breaker_open;
    m_state.window = tx.window;
    m_state.in_flight = tx.in_flight;
    m_snapshot = snapshot;
    m_settings_view = m_settings;
    xSemaphoreGive(m_state_mutex);
}

ArqLink::Snapshot ArqLink::snapshot() const
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    const Snapshot out = m_snapshot;
    xSemaphoreGive(m_state_mutex);
    return out;
}
