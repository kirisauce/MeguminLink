#pragma once
#ifndef __MEAPROTO_DAEMON_HPP__
#define __MEAPROTO_DAEMON_HPP__

#include "meaproto/config.hpp"
#include "meaproto/core.hpp"
#include "meaproto/sink.hpp"
#include "meaproto/transport.hpp"
#include "meaproto/types.hpp"

#include <atomic>
#include <limits>
#include <span>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace meaproto
{
/** 取会话标识 */
u32 random_epoch(void *ctx) noexcept;

/**
 * 一条连接的运行外壳：把具体 Transport 绑到 Core 上，并跑在一条 FreeRTOS 任务里。
 *
 * 协议状态全在 Core 里，本类只负责把它跑在任务上，并把外部调用转成任务上下文
 * 里的动作。
 */
template <Transport T> class Daemon final
{
  public:
    static constexpr UBaseType_t TASK_PRIORITY = 6;
    static constexpr usize TASK_STACK = 6144;

    Daemon(T &transport, Sink &sink, std::span<const ChannelSpec> channels, const Config &config = {}) noexcept;
    ~Daemon();

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    void start(UBaseType_t priority = TASK_PRIORITY, usize stack = TASK_STACK);
    void stop();

    /** 请求发起握手；已连接或连接中幂等 */
    void open();

    /** 请求断开 */
    void close();

    /**
     * 提交一条消息。任务未启动时返回 InvalidState，其余结果由协议层判定。
     *
     * 与任务循环共用协议锁，因此不能与 step()/receive() 并发；Sink 回调运行在
     * 持锁的上下文里，那里不得调用本接口。
     */
    SendResult send(ChannelId channel, std::span<const u8> data);

    /** 该通道发送队列是否还有空位（背压用） */
    bool can_send(ChannelId channel) const;

    /** 切换通道可靠性策略；切换会清空该通道的收发状态，两端必须同时切换 */
    bool set_reliability(ChannelId channel, Reliability reliability);

    /** 发一个 Ping 采样 RTT；未连接时返回 false */
    bool ping();

    bool connected() const;
    bool connecting() const;
    Stats snapshot() const;

  private:
    enum class Request : u8
    {
        None,
        Open,
        Close,
    };

    static_assert(T::MTU <= static_cast<usize>(std::numeric_limits<u16>::max()),
                  "transport MTU exceeds the u16 handshake field");
    /** 队列元素与接收缓冲的尺寸 */
    static constexpr usize RX_ITEM = sizeof(u16) + T::MTU;

    static void task_entry(void *ctx);
    static bool send_hook(void *ctx, std::span<const u8> data) noexcept;
    static void receive_hook(void *ctx, const u8 *data, usize len) noexcept;

    /** Transport 接收回调的落点：拷进队列并唤醒任务（要求回调串行，见 Transport 约定） */
    void push_frame(const u8 *data, usize len);
    /** 刷新统计快照并打状态迁移日志（任务上下文） */
    void publish();

    /** 协议锁：任务循环与外部接口共用（互斥量创建失败时退化为无锁） */
    void lock_core() const;
    void unlock_core() const;

    void run();
    void apply_requests(u32 now);
    void drain_rx(u32 now);
    void log_stats(u32 now);
    void wake();

    // ---- 绑定的对象 ----
    T &m_transport;

    /** 通道存储：按通道表算出需求后一次性分配，构造时交给协议层 */
    usize m_storage_bytes = 0;
    u8 *m_storage = nullptr;

    Core m_core;

    // ---- 接收通路 ----
    QueueHandle_t m_rx_queue = nullptr; /**< 构造时分配、析构时释放 */
    u8 m_rx_depth = 1;                  /**< 队列深度 */
    u16 m_rx_item = 0;                  /**< 队列元素尺寸 */
    u8 m_rx_staging[RX_ITEM] = {};      /**< 接收回调独占的拷贝缓冲 */
    u8 m_rx_frame[RX_ITEM] = {};        /**< 任务上下文读出队列元素用 */
    std::atomic<u32> m_rx_dropped{0};   /**< 超长帧与入队失败的丢弃数 */

    // ---- 任务与外部请求 ----
    SemaphoreHandle_t m_core_mutex = nullptr;            /**< 协议状态互斥：任务循环与外部接口共用 */
    SemaphoreHandle_t m_stopped = nullptr;               /**< 任务退出信号量 */
    std::atomic<TaskHandle_t> m_task{nullptr};           /**< 停止后为空，外部只读它来唤醒 */
    std::atomic<bool> m_stopping{false};                 /**< 已请求任务退出 */
    std::atomic<Request> m_request{Request::None};       /**< 后写覆盖先写 */
    std::atomic<Core::State> m_state{Core::State::Idle}; /**< 只有任务写，外部无锁读 */

    // ---- 统计与日志 ----
    mutable portMUX_TYPE m_snapshot_lock = portMUX_INITIALIZER_UNLOCKED; /**< 多个计数器要一起成立 */
    Stats m_snapshot{};
    Core::State m_logged_state = Core::State::Idle; /**< 上次打印的状态 */
    u32 m_next_stats_ms = 0;                        /**< 下次统计时刻 */
};
} // namespace meaproto

#include "meaproto/internal/daemon_impl.hpp"

#endif // __MEAPROTO_DAEMON_HPP__
