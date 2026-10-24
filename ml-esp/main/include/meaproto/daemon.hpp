#pragma once
#ifndef __MEAPROTO_DAEMON_HPP__
#define __MEAPROTO_DAEMON_HPP__

#include "meaproto/config.hpp"
#include "meaproto/core.hpp"
#include "meaproto/sink.hpp"
#include "meaproto/transport.hpp"
#include "meaproto/types.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <span>

namespace meaproto
{
/** 取会话标识：固件用 esp_random()，宿主测试不走这里 */
u32 random_epoch(void *ctx) noexcept;

/**
 * Daemon 的非模板部分：任务、接收队列、统计快照与跨任务请求。
 *
 * 协议状态全在 Core 里，本类只负责把它跑在一条 FreeRTOS 任务上，并把外部调用
 * 转成任务上下文里的动作。
 */
class DaemonImpl
{
  public:
    static constexpr UBaseType_t TASK_PRIORITY = 6;
    static constexpr usize TASK_STACK = 6144;

    DaemonImpl(Sink &sink, std::span<const ChannelSpec> channels, const Config &config, u16 mtu,
               const CoreHooks &hooks) noexcept;
    ~DaemonImpl();

    DaemonImpl(const DaemonImpl &) = delete;
    DaemonImpl &operator=(const DaemonImpl &) = delete;

    void start(UBaseType_t priority = TASK_PRIORITY, usize stack = TASK_STACK);
    void stop();

    /** 请求发起握手；已连接或连接中幂等 */
    void open();

    /** 请求断开 */
    void close();

    bool connected() const;
    bool connecting() const;
    Stats snapshot() const;

  protected:
    /** Transport 接收回调的落点：拷进队列并唤醒任务（要求回调串行，见 Transport 约定） */
    void push_frame(const u8 *data, usize len);

    /** 刷新统计快照并打状态迁移日志（任务上下文） */
    void publish();

  private:
    enum class Request : u8
    {
        None,
        Open,
        Close,
    };

    static void task_entry(void *ctx);

    void run();
    void apply_requests(u32 now);
    void drain_rx(u32 now);
    void log_stats(u32 now);
    void wake();

    Core m_core;

    /** 队列与退出信号量在构造时分配、析构时释放：stop() 只停任务，不拆缓冲 */
    QueueHandle_t m_rx_queue = nullptr;
    SemaphoreHandle_t m_stopped = nullptr;

    /** 任务句柄：外部任务只读它来唤醒，stop() 先摘句柄再唤醒 */
    std::atomic<TaskHandle_t> m_task{nullptr};
    /** 最近一次外部请求：后写的覆盖先写的，避免 open/close 同拍时顺序歧义 */
    std::atomic<Request> m_request{Request::None};
    std::atomic<bool> m_stopping{false};
    std::atomic<u32> m_rx_dropped{0};

    /** 对外可见的连接状态：只有任务写，任何人都能无锁读 */
    std::atomic<Core::State> m_state{Core::State::Idle};

    /** 统计副本：多个计数器要一起成立，因此用临界区保护 */
    mutable portMUX_TYPE m_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
    Stats m_snapshot{};

    u8 m_rx_depth = 1;
    u16 m_rx_item = 0;
    Core::State m_logged_state = Core::State::Idle;
    u32 m_next_stats_ms = 0;
    /** Transport 接收回调上下文独占的拷贝缓冲 */
    u8 m_rx_staging[sizeof(u16) + MAX_MTU] = {};
    /** 任务上下文读出队列元素用 */
    u8 m_rx_frame[sizeof(u16) + MAX_MTU] = {};
};

/** 一条连接的运行外壳：把具体 Transport 绑到 DaemonImpl 上 */
template <Transport T> class Daemon final : public DaemonImpl
{
  public:
    Daemon(T &transport, Sink &sink, std::span<const ChannelSpec> channels, const Config &config = {})
        : DaemonImpl(sink, channels, config, transport_mtu(transport),
                     CoreHooks{this, &Daemon::send_hook, &random_epoch}),
          m_transport(transport)
    {
        m_transport.set_receive_handler(&Daemon::receive_hook, this);
    }

  private:
    static u16 transport_mtu(T &transport) noexcept
    {
        const usize mtu = transport.mtu();
        return static_cast<u16>(mtu < MAX_MTU ? mtu : MAX_MTU);
    }

    static bool send_hook(void *ctx, std::span<const u8> data) noexcept
    {
        return static_cast<Daemon *>(ctx)->m_transport.send(data);
    }

    static void receive_hook(void *ctx, const u8 *data, usize len) noexcept
    {
        static_cast<Daemon *>(ctx)->push_frame(data, len);
    }

    T &m_transport;
};
} // namespace meaproto

#endif // __MEAPROTO_DAEMON_HPP__
