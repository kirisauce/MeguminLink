#pragma once
#ifndef __MEGUMINLINK_ARQ_LINK_HPP__
#define __MEGUMINLINK_ARQ_LINK_HPP__

#include "esp_err.h"
#include "meguminlink/wireless/arq.hpp"
#include "meguminlink/common.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstddef>

namespace mlink
{
/**
 * 可靠传输参数。整份参数两端必须一致，否则窗口、重传预算这些会各按各的算，
 * 测出来的行为解释不了。
 *
 * generation 是"参数代数"：主机每次下发参数 +1，落 NVS。两端参数不一致时靠它
 * 判断该听谁的——代数大的那份更新；代数相同（比如两块板一起上电）则用 MAC 大小
 * 做确定性平局，保证两端算出同一个赢家，最终收敛到同一组参数。
 */
struct ArqSettings
{
    u32 generation;       /**< 参数代数：每次主机下发 +1（落 NVS） */
    u8 enable;            /**< 运行时开关：0 = 绕过核心，等价尽力而为（帧格式不变） */
    u8 window;            /**< 发送窗口（本批固定 1） */
    u8 max_retx;          /**< 单条消息重传次数上限 */
    u8 breaker_failures;  /**< 连续多少条重传耗尽后熔断，0 = 不熔断 */
    u16 rto_min_ms;       /**< RTO 下限 */
    u16 initial_rto_ms;   /**< 还没有 RTT 样本时的初始 RTO */
    u16 rto_max_ms;       /**< RTO 上限 */
    u16 delayed_ack_ms;   /**< 没有数据可捎带时补纯 ACK 的延迟 */
    u16 breaker_probe_ms; /**< 熔断期间放行试探的间隔 */
};

/** 参数是否合法（越界值一律拒绝，免得写进 NVS 或按它运行） */
bool arq_settings_valid(const ArqSettings &settings);

/** 出厂默认参数（与实测调优后的取值一致） */
ArqSettings arq_default_settings();

/** 参数 → 算法配置 */
arq::Config arq_config_from(const ArqSettings &settings);

/** 从 NVS 读回参数；无记录或失败时返回默认值 */
ArqSettings arq_settings_load();

/** 参数落 NVS：所有键在同一个 commit 里写入 */
esp_err_t arq_settings_store(const ArqSettings &settings);

/**
 * 无线链路的可靠传输适配层：把 arq::Sender / arq::Receiver 接到实际的收发上，
 * 并管理会话握手、待发确认与重复帧重放的排队。
 *
 * 它与帧的编解码无关——只认"载荷 + 序号 + 标志"，帧封成什么样、扩展头怎么排由
 * wireless_link 决定。因此它既不需要认识 Packet，也不会和 wireless/link.hpp 互相
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
                                u8 meta, u16 ack, u16 sack, u8 window);

    /** 发一个纯确认帧（没有数据可捎带时） */
    using AckFn = void (*)(void *ctx, u16 ack, u16 sack, u8 window);

    /**
     * 发一个会话重置帧。重置帧同时携带本端的参数与确认/窗口通告：参数靠它传给
     * 对端（请求会周期性重发直到握手完成，因此带重传保证），而扩展头的 window
     * 在所有帧上的含义都一样（本端还能收多少），漏填会被对端当成"窗口为 0"。
     *
     * @param echo true = 对端请求的回显（收到方不再回），false = 本端的重置请求
     */
    using ResetFn = void (*)(void *ctx, u32 epoch, const ArqSettings &settings, bool echo, u16 ack, u16 sack,
                             u8 window);

    /**
     * 按序交付一条可靠消息。对端 MAC 一并给出：本回调是在持锁状态下被调用的，
     * 再回头调本层的 peer_mac() 会去抢同一把非递归互斥量而死锁。meta 是这条消息
     * 随帧携带的 1 字节元数据，乱序补齐时按消息逐条还原（见 ReorderStore）。
     *
     * 乱序补齐时它会在同一次 on_message 里按序被连续调用多次（补齐几帧就几次），
     * 实现必须接得住连续交付，不能假设"一次收帧只交付一条"。
     */
    using DeliverFn = void (*)(void *ctx, const u8 *peer_mac, u16 seq, const u8 *payload, u16 len, u8 meta);

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
     * @param settings     参数（通常来自 arq_settings_load()）
     * @param payload_cap  单条可靠消息的载荷上限（≤ MAX_PAYLOAD）
     */
    esp_err_t init(const ArqSettings &settings, size_t payload_cap);

    void set_callbacks(const Callbacks &callbacks);

    /** 设定会话对端；对端变化会重置会话（两端序号基准重新对齐） */
    void set_peer(const u8 *peer_mac, u32 now_ms);

    /** 清除会话对端（离开无线模式/配对丢失） */
    void clear_peer();

    bool has_peer() const;

    /** 会话握手是否已完成（两端序号基准已对齐） */
    bool established() const;

    /**
     * 取出一条"重传耗尽、已判定失败"的消息序号。
     *
     * 失败的消息不会再有响应，调用方据此把在途条目销账（策略是不回响应、让主机
     * 那条命令自己超时）。核心的事件回调跑在持锁上下文里，不能直接回调上层，
     * 因此这里改成查询式的小队列。
     * @return false = 当前没有待处理的失败
     */
    bool take_failed(u16 *seq_out);

    /**
     * 现在能不能提交新消息（会话已对齐、未熔断、窗口还有空位）。
     *
     * 调用方据此形成背压：不能提交时先别取新命令，让命令留在队列里、由 USB 把主机
     * 挡回去。取出来再丢弃会让主机那条命令永远等不到响应，只能一路超时。
     * 未启用可靠传输时没有窗口概念，返回真。
     */
    bool can_submit() const;

    /**
     * 应用一份新参数（主机下发）：立即生效、换新 epoch 重置会话，新参数随会话
     * 重置帧传给对端；并把参数标记为待落盘（NVS 写入由主循环做）。
     */
    esp_err_t apply_settings(const ArqSettings &settings, u32 now_ms);

    /** 当前参数快照（配置读取与落盘用） */
    ArqSettings settings() const;

    /** 参数被主机或对端改动过：主循环取走并落盘（取走即清标记） */
    bool take_settings_dirty();

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
    void on_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u8 meta, u32 now_ms);

    /** 对端的会话重置请求（携带对端参数） */
    void on_reset(u32 peer_epoch, const ArqSettings &peer_settings, bool echo, u32 now_ms);

    // ---- 任务上下文 ----

    /** 驱动重传、纯确认、重放与会话握手；所有出向帧都在这里发出 */
    void poll(u32 now_ms);

    /**
     * 下一个需要 poll 的时刻（毫秒绝对值）；没有待办时返回 0。
     * 调用方据此设置等待超时，空闲时不必轮询。
     */
    u32 next_deadline_ms() const;

    /** 提交一条可靠消息 */
    Submit submit(const u8 *payload, u16 len, u8 meta, u32 now_ms, u16 *seq_out);

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
    static void core_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before,
                              u8 meta);

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
            Reset,   /**< 会话重置帧（请求或回显，见 reset_echo） */
        };

        Kind kind;
        bool retransmit;
        bool gap_before;
        bool reset_echo; /**< 仅重置动作使用：true = 回显，false = 请求 */
        u16 seq;
        u16 len;
        u8 meta; /**< 消息动作携带的元数据（DAP 命令用它承载主机侧传输通道） */
        u16 ack;
        u16 sack;
        u8 window;
        u32 epoch;
        ArqSettings settings; /**< 仅重置动作使用 */
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

    /** 造一个会话重置动作（带上本端参数与接收侧的确认、窗口通告）；echo 语义见 ResetFn */
    Action make_reset_action_locked(bool echo) const;

    /** 参数是否该采纳对端那份：代数大的赢，代数相同用 MAC 做确定性平局 */
    bool should_adopt_settings_locked(const ArqSettings &peer) const;

    /** 按当前参数重建核心（Sender/Receiver 的配置随之生效） */
    void reconfigure_locked();

    /**
     * 按序交付连续区间 [first, last] 的载荷（须在持锁时调用）。
     *
     * first 那一帧的载荷由调用方直接给出（就是本次收到的帧），其余从乱序缓冲里
     * 取出并在交付后释放；区间内没有载荷的序号（对端声明放弃的空洞）跳过。
     */
    void deliver_range_locked(u16 first, const u8 *first_payload, u16 first_len, u8 first_meta, u16 last);

    /** 记录一条失败消息的序号（须在持锁时调用，供核心事件回调使用） */
    void record_failed_locked(u16 seq);

    ArqSettings m_settings = {};
    arq::Config m_config = {};
    size_t m_payload_cap = 0;
    u8 m_own_mac[6] = {};
    bool m_settings_dirty = false;

    arq::Sender m_sender;
    arq::Receiver m_receiver;
    u8 m_tx_storage[static_cast<size_t>(arq::MAX_WINDOW) * MAX_PAYLOAD] = {};

    /** 接收侧的乱序载荷缓冲：窗口大于 1 时补齐落在游标之后的帧 */
    arq::ReorderStore m_rx_reorder;
    u8 m_rx_storage[static_cast<size_t>(arq::MAX_WINDOW) * MAX_PAYLOAD] = {};

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

    /** 失败消息序号的小队列：核心事件回调只入队，上层用 take_failed 取走销账 */
    u16 m_failed_seqs[arq::MAX_WINDOW] = {};
    u8 m_failed_head = 0;
    u8 m_failed_count = 0;

    u32 m_replayed = 0;
    u32 m_replay_missed = 0;
};

} // namespace mlink

#endif // __MEGUMINLINK_ARQ_LINK_HPP__
