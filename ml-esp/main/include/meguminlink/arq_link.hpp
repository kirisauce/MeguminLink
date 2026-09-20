#pragma once
#ifndef __MEGUMINLINK_ARQ_LINK_HPP__
#define __MEGUMINLINK_ARQ_LINK_HPP__

#include "esp_err.h"
#include "meguminlink/arq.hpp"
#include "meguminlink/common.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstddef>

namespace mlink
{
/**
 * 无线链路的可靠传输适配层：把 arq::Sender / arq::Receiver 接到实际的收发上，
 * 并管理会话握手、待发确认与重复帧重放的排队。
 *
 * 它与帧的编解码无关——只认"载荷 + 序号 + 标志"，帧封成什么样、扩展头怎么排由
 * wireless_link 决定。因此它既不需要认识 Packet，也不会和 wireless_link.hpp 互相
 * 包含。
 *
 * 上下文约定是本层最要紧的一条：
 * - on_ack / on_message / on_reset 由 ESP-NOW 接收回调调用（esp_wifi 任务），**只
 *   更新状态与待发标记，绝不发送**——接收回调里发帧会重入 WiFi 驱动；
 * - poll / submit / reset_session 由任务上下文调用（USB 侧 dap_task、主循环），
 *   所有出向帧都只在这里发出。
 * 核心状态由一把互斥量串行化，两个上下文因此可以并存。
 *
 * 输出全部走回调：可靠消息、纯确认、会话重置、按序交付各一个。回调在持锁状态下
 * 被调用，因此它们必须只做非阻塞的事（送空口、入队、写响应缓冲），且不得回调本层。
 */
class ArqLink
{
  public:
    /**
     * 可靠传输能承载的载荷上限。取"ARQ 关闭时 DAP 单帧载荷"的旧上界，作为本层
     * 静态缓冲的尺寸依据；实际生效值由 init() 传入（ARQ 打开时更小），调用方用
     * static_assert 保证传进来的值不超过它。
     */
    inline static constexpr size_t MAX_PAYLOAD = 1467;

    /** 会话握手超时：超过这么久还没对齐，调用方应把命令按失败处理 */
    inline static constexpr u32 HANDSHAKE_TIMEOUT_MS = 5000;

    /** 会话未建立时重发重置请求的间隔 */
    inline static constexpr u32 RESET_INTERVAL_MS = 1000;

    /** 回应对方重置请求的最小间隔（挡住对端重发造成的回显风暴） */
    inline static constexpr u32 RESET_REPLY_MIN_MS = 200;

    /** 提交结果 */
    enum class Submit : u8
    {
        Ok,             /**< 已进入窗口并发出 */
        NotEstablished, /**< 会话尚未对齐，调用方应等握手或按失败处理 */
        WindowFull,     /**< 窗口已满（含对端通告的接收窗口为 0） */
        BreakerOpen,    /**< 熔断中，快速失败 */
        Invalid,        /**< 载荷非法 */
    };

    /** 发一条可靠消息（首传 / 重传 / 重放） */
    using TransmitFn = void (*)(void *ctx, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before,
                                u16 ack, u16 sack, u8 window);

    /** 发一个纯确认帧（没有数据可捎带时） */
    using AckFn = void (*)(void *ctx, u16 ack, u16 sack, u8 window);

    /**
     * 发一个会话重置请求。重置帧同样携带本端接收侧的确认与窗口通告——扩展头的
     * window 字段在所有帧上的含义都一样（本端还能收多少），漏填会被对端当成
     * "对端窗口为 0"，于是它一条命令都发不出来。
     */
    using ResetFn = void (*)(void *ctx, u32 epoch, u16 ack, u16 sack, u8 window);

    /**
     * 按序交付一条可靠消息。对端 MAC 一并给出：本回调是在持锁状态下被调用的，
     * 再回头调本层的 peer_mac() 会去抢同一把非递归互斥量而死锁。
     */
    using DeliverFn = void (*)(void *ctx, const u8 *peer_mac, u16 seq, const u8 *payload, u16 len);

    /** 本层的全部输出 */
    struct Callbacks
    {
        void *ctx = nullptr;
        TransmitFn transmit = nullptr;
        AckFn send_ack = nullptr;
        ResetFn send_reset = nullptr;
        DeliverFn deliver = nullptr;
    };

    /**
     * @param config       算法参数
     * @param payload_cap  单条可靠消息的载荷上限（≤ MAX_PAYLOAD）
     */
    esp_err_t init(const arq::Config &config, size_t payload_cap);

    void set_callbacks(const Callbacks &callbacks);

    /** 设定会话对端；对端变化会重置会话（两端序号基准重新对齐） */
    void set_peer(const u8 *peer_mac, u32 now_ms);

    /** 清除会话对端（离开无线模式/配对丢失） */
    void clear_peer();

    bool has_peer() const;

    /** 会话握手是否已完成（两端序号基准已对齐） */
    bool established() const;

    /** 是否还有收到但未确认的帧（调用方据此把 delayed ACK 的到期时刻算进等待超时） */
    bool ack_pending() const;

    /** 捎带用：出向帧都带扩展头，因此每次发送都会取走一次确认信息 */
    struct Piggyback
    {
        u16 ack;
        u16 sack;
        u8 window;
    };

    /**
     * 取走本端接收侧的确认信息，供非可靠帧（Ping、串口数据、控制帧）捎带。
     * 取用即视为确认义务已随该帧完成，因此会清掉待发标记。
     */
    Piggyback take_piggyback();

    /** 会话对端 MAC；返回 false 表示当前没有对端 */
    bool peer_mac(u8 *out) const;

    // ---- 接收路径（ESP-NOW 回调上下文；只置标记，不发送）----

    /** 任意帧捎带的确认信息 */
    void on_ack(u16 ack, u16 sack, u8 window, u32 now_ms);

    /** 一条可靠消息（DAP 命令或响应，取决于是链路的哪一端） */
    void on_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u32 now_ms);

    /** 对端的会话重置请求 */
    void on_reset(u32 peer_epoch, u32 now_ms);

    // ---- 任务上下文 ----

    /** 驱动重传、纯确认、重放与会话握手；所有出向帧都在这里发出 */
    void poll(u32 now_ms);

    /**
     * 下一个需要 poll 的时刻（毫秒绝对值）；没有待办时返回 0。
     * 调用方据此设置等待超时，空闲时不必轮询。
     */
    u32 next_deadline_ms() const;

    /** 提交一条可靠消息 */
    Submit submit(const u8 *payload, u16 len, u32 now_ms, u16 *seq_out);

    /**
     * 无 USB 侧专用：提交一条 DAP 响应，并记下它回应的是哪条命令。
     * 对端重传那条命令时据此找回这条响应重放，从而不必重新执行命令。
     */
    Submit submit_response(u16 cmd_seq, const u8 *payload, u16 len, u32 now_ms);

    /** 会话重置：清窗口、重置序号基准，重新开始握手 */
    void reset_session(u32 now_ms);

    /** 累计计数（统计上报与日志用） */
    struct Snapshot
    {
        u32 tx;              /**< 首次发出的可靠消息数 */
        u32 retx;            /**< 重传次数 */
        u32 acked;           /**< 被确认的消息数 */
        u32 failed;          /**< 重传耗尽被丢弃的消息数 */
        u32 rx_delivered;    /**< 按序交付的帧数 */
        u32 rx_duplicate;    /**< 重复帧数 */
        u32 rx_out_of_window;/**< 超窗被拒的帧数 */
        u32 rx_skipped;      /**< 因对端放弃空洞而跳过的序号数 */
        u32 replayed;        /**< 因重复命令而重放响应的次数 */
        u32 replay_missed;   /**< 重放未命中（响应已被确认或尚未产生）的次数 */
        u32 breaker_trips;   /**< 熔断触发次数 */
        u32 srtt_ms;
        u32 rto_ms;
        u8 in_flight;
        bool established;
        bool breaker_open;
    };

    Snapshot snapshot() const;

  private:
    /** 核心的发送回调：补上本端接收侧的确认信息后转交给调用方 */
    static void core_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before);

    /** 核心的事件回调：打日志（计数直接从核心快照读，不在这里累加） */
    static void core_event(void *raw, arq::Event event, u16 seq, u32 now_ms);

    /**
     * 一次待发动作。锁内只登记，出锁后才真正发出去——绝不能持着本层的锁去调
     * WiFi 驱动：ESP-NOW 接收回调跑在 WiFi 任务里、也要抢这把锁，而驱动调用要
     * 等 WiFi 任务，两边一对上就是整机僵死。
     */
    struct Action
    {
        enum class Kind : u8
        {
            Message, /**< 可靠消息（首传 / 重传 / 重放） */
            Ack,     /**< 纯确认帧 */
            Reset,   /**< 会话重置请求 */
        };

        Kind kind;
        bool retransmit;
        bool gap_before;
        u16 seq;
        u16 len;
        u16 ack;
        u16 sack;
        u8 window;
        u32 epoch;
    };

    static constexpr size_t MAX_ACTIONS = 4;

    void lock() const;
    void unlock() const;

    /** 锁内登记一次待发动作（队列满则丢弃并告警） */
    void queue_locked(const Action &action);

    /** 出锁后逐条发出（入参是锁内拷出来的副本，不共享状态） */
    void dispatch(const Action *actions, size_t count);

    /** 把到期的纯确认登记成待发动作（须在持锁时调用） */
    void queue_ack_locked(u32 now_ms);

    /** 锁内动作收集完毕：拷出副本、清空队列，调用方随后 dispatch */
    size_t take_actions_locked(Action *out);

    /** 造一个会话重置动作（带上本端接收侧的确认与窗口通告） */
    Action make_reset_action_locked() const;

    arq::Config m_config = {};
    size_t m_payload_cap = 0;

    arq::Sender m_sender;
    arq::Receiver m_receiver;
    u8 m_tx_storage[static_cast<size_t>(arq::MAX_WINDOW) * MAX_PAYLOAD] = {};

    mutable SemaphoreHandle_t m_mutex = nullptr;
    Callbacks m_callbacks = {};

    u8 m_peer_mac[6] = {};
    bool m_peer_known = false;

    /** 本端 epoch：每次上电与会话重置随机生成，让对端能区分"新会话"与"迟到的旧帧" */
    u32 m_epoch = 0;
    u32 m_peer_epoch = 0;
    bool m_peer_epoch_known = false;
    bool m_established = false;
    u32 m_next_reset_tx_ms = 0;

    /** delayed ACK：收到需要确认的帧后置位，攒够 delayed_ack_ms 仍无帧可捎带就补一个纯确认 */
    bool m_ack_scheduled = false;
    u32 m_ack_due_ms = 0;

    /** 重复帧触发的重放：接收上下文只记序号，真正的重发在 poll 里做 */
    bool m_replay_pending = false;
    u16 m_replay_cmd_seq = 0;

    /** 收到对端的重置请求需要回显：接收上下文只置标记，真正发出在 poll 里做 */
    bool m_reset_reply_pending = false;
    u32 m_reset_reply_due_ms = 0;

    /** 锁内登记、出锁发送的动作队列 */
    Action m_actions[MAX_ACTIONS] = {};
    size_t m_action_count = 0;

    /** 命令序号 → 响应序号（无 USB 侧重放用；USB 侧不接收命令，不使用） */
    u16 m_replay_map[arq::MAX_WINDOW] = {};
    bool m_replay_map_valid[arq::MAX_WINDOW] = {};

    u32 m_replayed = 0;
    u32 m_replay_missed = 0;
};

} // namespace mlink

#endif // __MEGUMINLINK_ARQ_LINK_HPP__
