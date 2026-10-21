#pragma once
#ifndef __MEGUMINLINK_ARQ_LINK_HPP__
#define __MEGUMINLINK_ARQ_LINK_HPP__

#include "esp_err.h"
#include "meguminlink/common.hpp"
#include "meguminlink/wireless/arq.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstddef>

namespace mlink
{
/**
 * 单调毫秒时基：ARQ 层与链路层共用同一个时间口径（FreeRTOS tick 换算）。
 *
 * 放在这里而不是 common.hpp：那个头被纯算法核心 arq.hpp include，必须保持零
 * ESP-IDF 依赖（宿主机单测要能直接编它）。
 */
inline u32 now_ms()
{
    return static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ---- 线格式 ----

/**
 * ARQ 帧：无线链路上只有这一种帧。头部 8 字节固定，之后是载荷，字段一律大端。
 * 载荷形态由 flags 的形态位决定：数据帧的载荷是一个业务包（[type:1B][body]），
 * 纯确认帧没有载荷，会话重置帧的载荷是本层的 23 字节重置信息。
 *
 * 业务包对本层是不透明字节：本层不解析它，只负责可靠地把它送到对面。
 * 单帧上限是 ESP-NOW v2 的 1470B，扣掉头部即本层能承载的载荷上限。
 */
inline constexpr size_t ARQ_HEADER_SIZE = 8;

/** ARQ 帧 flags 位 */
inline constexpr u8 ARQ_FLAG_RELIABLE = 0x01;   /**< 载荷是可靠消息（占序号、进窗口、可按序交付） */
inline constexpr u8 ARQ_FLAG_RETRANSMIT = 0x02; /**< 本帧是重传 */
inline constexpr u8 ARQ_FLAG_GAP_BEFORE = 0x04; /**< 本序号之前的空洞已被发送端放弃 */
inline constexpr u8 ARQ_FLAG_ACK_ONLY = 0x08;   /**< 无载荷的纯确认 */
inline constexpr u8 ARQ_FLAG_RESET = 0x10;      /**< 载荷是会话重置信息 */

/** 载荷形态位：两位都不置 = 载荷是业务包 */
inline constexpr u8 ARQ_FLAG_KIND_MASK = ARQ_FLAG_ACK_ONLY | ARQ_FLAG_RESET;

/** 只对数据帧有意义的位（与形态位互斥） */
inline constexpr u8 ARQ_FLAG_MESSAGE_MASK = ARQ_FLAG_RELIABLE | ARQ_FLAG_RETRANSMIT | ARQ_FLAG_GAP_BEFORE;

/** 本层能承载的单条载荷上限（单帧上限 1470B 减去头部） */
inline constexpr size_t ARQ_MAX_PAYLOAD = 1462;

/** 一整帧的上限（含头部），接收通道按它开槽 */
inline constexpr size_t ARQ_FRAME_MAX_SIZE = ARQ_HEADER_SIZE + ARQ_MAX_PAYLOAD;

/** 会话重置载荷的线格式长度：epoch 4 + 代数 4 + 三个 1B 参数 + 五个 u16 参数 + 标志 2 */
inline constexpr size_t ARQ_RESET_PAYLOAD_SIZE = 4 + 4 + 3 + 10 + 2;

/** ARQ 帧头：线格式的头 8 个字节 */
struct ArqHeader
{
    u16 seq;    /**< 本帧序号（可靠消息有效） */
    u16 ack;    /**< 累积确认号：已连续收到 < ack 的全部帧 */
    u16 sack;   /**< 选择确认位图：第 i 位对应 seq == ack + 1 + i */
    u8 window;  /**< 本端接收窗口通告（0 = 请对端暂停发送） */
    u8 flags;   /**< ARQ_FLAG_* */
};

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
    u8 window;            /**< 发送窗口 */
    u8 max_retx;          /**< 单条消息重传次数上限（单条消息可单独覆盖） */
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
 * 无线链路的 ARQ 层：把 arq::Sender / arq::Receiver 接到实际的收发上，并管理
 * 会话握手、待发确认与重复帧重放。
 *
 * 它独占一个任务：全部 ARQ 状态只在那个任务里被读写，因此核心与状态机不需要
 * 互斥量。外部只有三件事：
 * 1. 提交——业务包字节 + 目标 MAC + 可靠性等级（见 submit），投进待发环即返回；
 * 2. 入向——ESP-NOW 回调把原始帧投进接收环（见 on_frame）；
 * 3. 查询——会话是否对齐、能不能提交、失败序号、累计计数（走对外缓存）。
 *
 * 与帧的编解码无关：只认"载荷 + 序号 + 标志"，业务包封成什么样由 wireless_link
 * 决定。因此它既不需要认识 Packet，也不会和 wireless/link.hpp 互相包含。
 *
 * 可靠性由提交方给出（见 Reliability），本层不按载荷内容猜：可靠消息占序号、
 * 进窗口、按重传预算保达并按序交付；不可靠消息直接发、收到即交付，只借用数据帧
 * 捎带确认。
 *
 * 输出全部走回调，且都在任务上下文里被调用：裸发送与按序交付各一个。交付回调
 * 必须只做非阻塞的事（入队、写缓冲），它有可能会推迟重传定时器。
 */
class ArqLink
{
  public:
    /** 一条消息的可靠性等级，由提交方给出 */
    enum class Reliability : u8
    {
        Unreliable, /**< 不占序号、不进窗口：直接发，收到即交付 */
        Reliable,   /**< 占序号、进窗口：按重传预算保达，接收端按序交付 */
    };

    /** 提交时携带的传输属性（与载荷内容无关） */
    struct TxOptions
    {
        Reliability reliable = Reliability::Reliable;
        /** 本条消息的重传次数上限；0 = 用配置里的 max_retx */
        u8 retx_limit = 0;
        /** 是否参与注入丢帧（配对、探测、控制帧应当置 false） */
        bool loss_injectable = true;
    };

    /** 提交结果 */
    enum class Submit : u8
    {
        Ok,             /**< 已进入待发环 */
        Invalid,        /**< 载荷为空或超过本层上限 */
        NoPeer,         /**< 还没有会话对端 */
        NotEstablished, /**< 可靠消息且会话尚未对齐 */
        BreakerOpen,    /**< 熔断中，此刻提交会被快速失败 */
        QueueFull,      /**< 待发环已满，调用方应稍后重试或按自己的策略丢弃 */
    };

    /** 本层的全部输出 */
    struct Callbacks
    {
        void *ctx = nullptr;

        /**
         * 裸发送：把这段缓冲发给这个 MAC。本层不解释缓冲内容，链路侧的发送
         * 原语同样只看缓冲区。
         *
         * @param reliable        本帧是否承载可靠消息（链路据此让路与退避）
         * @param loss_injectable 本帧是否参与注入丢帧（测试设施）
         */
        void (*send_raw)(void *ctx, const u8 *dest_mac, const u8 *data, size_t len, bool reliable,
                         bool loss_injectable) = nullptr;

        /**
         * 一条消息的载荷：可靠消息按序到达，不可靠消息收到即交付。对端 MAC 一并
         * 给出（本回调在任务里被调用，回头调本层的 peer 查询没有必要）。
         */
        void (*deliver)(void *ctx, const u8 *src_mac, u16 seq, const u8 *payload, u16 len) = nullptr;

        /**
         * 每轮的空闲钩子：链路层在这里排空自己的尽力而为出向数据（串口字节流）。
         * 本层不参与限速与重试，发不出去的由链路层留到下一轮。
         */
        void (*poll)(void *ctx) = nullptr;
    };

    /**
     * 本层能承载的单条载荷上限。业务包（含自己的类型与长度字段）必须装得下它，
     * 否则提交会被判 Invalid；wireless_link 侧用 static_assert 把两者绑住。
     */
    inline static constexpr size_t MAX_PAYLOAD = ARQ_MAX_PAYLOAD;

    /** 会话握手超时：超过这么久还没对齐，调用方应把命令按失败处理 */
    inline static constexpr u32 HANDSHAKE_TIMEOUT_MS = 5000;

    /** 会话未建立时重发重置请求的间隔 */
    inline static constexpr u32 RESET_INTERVAL_MS = 1000;

    /** 回应对方重置请求的最小间隔（挡住对端重发造成的回显风暴） */
    inline static constexpr u32 RESET_REPLY_MIN_MS = 200;

    /** 待发环深度：可靠方向按窗口上限开槽，不可靠方向只吸收突发 */
    static constexpr size_t TX_RELIABLE_DEPTH = arq::MAX_WINDOW;
    static constexpr size_t TX_UNRELIABLE_DEPTH = 4;

    /** 接收环深度：回调只投递，解析与状态更新都在任务里 */
    static constexpr size_t RX_DEPTH = 3;

    /** 任务优先级：高于串口/USB 任务（5），低于 dap-wireless（24）与 esp_wifi（23） */
    static constexpr UBaseType_t TASK_PRIORITY = 6;
    static constexpr u32 TASK_STACK = 5120;

    /**
     * @param settings     参数（通常来自 arq_settings_load()）
     * @param payload_cap  单条载荷上限（≤ MAX_PAYLOAD，由调用方按业务包上限给出）
     */
    esp_err_t init(const ArqSettings &settings, size_t payload_cap);

    void set_callbacks(const Callbacks &callbacks);

    /**
     * 提交一条消息：投进待发环即返回，真正的封帧与发送在任务里做。
     *
     * 可靠性由调用方给出，本层不猜。可靠消息在环里排队等窗口：窗口满不会失败，
     * 只有环满才返回 QueueFull。会话未对齐或熔断中会立即拒绝，让调用方形成背压
     * 而不是把消息悄悄积压。
     *
     * @param dest_mac 目标 MAC（可靠消息必须是会话对端）
     * @param seq_out  可选：本条可靠消息的序号。可靠消息的序号按提交顺序分配，
     *                 与核心的发送序号同源同序，调用方据此把响应配回命令
     */
    Submit submit(const u8 *payload, u16 len, const u8 *dest_mac, const TxOptions &options,
                  u16 *seq_out = nullptr);

    /**
     * 提交一条 DAP 响应，并记下它回应的是哪条命令：对端重传那条命令时据此找回
     * 这条响应重放，从而不必重新执行命令。
     */
    Submit submit_response(u16 cmd_seq, const u8 *payload, u16 len, const u8 *dest_mac, const TxOptions &options);

    /** ESP-NOW 接收回调：只把原始帧投进接收环（环满则丢帧并计数） */
    void on_frame(const u8 *src_mac, const u8 *data, size_t len);

    /** 立刻唤醒任务：有新的尽力而为数据要排空时由链路层调用 */
    void wake();

    /** 设定会话对端；对端变化会重置会话（两端序号基准重新对齐） */
    void set_peer(const u8 *peer_mac);

    /** 清除会话对端（离开无线模式/配对丢失） */
    void clear_peer();

    bool has_peer() const;

    /** 会话握手是否已完成（两端序号基准已对齐） */
    bool established() const;

    /**
     * 现在能不能提交新消息（会话已对齐、窗口还有空位、且不在熔断里）。
     *
     * 调用方据此形成背压：不能提交时先别取新命令，让命令留在队列里、由 USB 把主机
     * 挡回去。取出来再丢弃会让主机那条命令永远等不到响应，只能一路超时。
     */
    bool can_submit() const;

    /**
     * 取出一条"重传耗尽、已判定失败"的消息序号。
     *
     * 失败的消息不会再有响应，调用方据此把在途条目销账（策略是不回响应、让主机
     * 那条命令自己超时）。
     */
    bool take_failed(u16 *seq_out);

    /**
     * 链路被证明可用（主循环收到对端 Pong 一类回显）：登记给任务清掉熔断与连续
     * 失败计数。任何上下文都可以调用。
     */
    void request_link_alive();

    /**
     * 应用一份新参数（主机下发）：立即置为待应用，任务换新 epoch 重置会话并把
     * 参数随会话重置帧传给对端；参数同时标记为待落盘（NVS 写入由主循环做）。
     */
    esp_err_t apply_settings(const ArqSettings &settings);

    /** 当前参数（对外缓存副本） */
    ArqSettings settings() const;

    /** 参数被主机或对端改动过：主循环取走并落盘（取走即清标记） */
    bool take_settings_dirty();

    /** 累计计数（统计上报与日志用） */
    struct Snapshot
    {
        u32 tx;               /**< 首次发出的可靠消息数 */
        u32 retx;             /**< 重传次数 */
        u32 acked;            /**< 被确认的消息数 */
        u32 failed;           /**< 重传耗尽被丢弃的消息数 */
        u32 rx_delivered;     /**< 按序交付的帧数 */
        u32 rx_duplicate;     /**< 重复帧数 */
        u32 rx_out_of_window; /**< 超窗被拒的帧数 */
        u32 rx_skipped;       /**< 因对端放弃空洞而被跳过的序号数 */
        u32 replayed;         /**< 因重复命令而重放响应的次数 */
        u32 replay_missed;    /**< 重放未命中（响应已被确认或尚未产生）的次数 */
        u32 tx_dropped;       /**< 待发环满而拒绝的提交数 */
        u32 rx_dropped;       /**< 接收环满而丢弃的帧数 */
        u32 breaker_trips;
        u32 srtt_ms;
        u32 rto_ms;
        u8 in_flight;
        bool established;
        bool breaker_open;
    };

    Snapshot snapshot() const;

  private:
    /** 待发环里的一条消息 */
    struct TxMessage
    {
        u8 dest_mac[6];
        u16 len;
        u16 cmd_seq; /**< 响应专用的"回应的是哪条命令" */
        u16 seq_hint; /**< 本条可靠消息的序号（按提交顺序预留，与核心分配的一致） */
        u8 retx_limit;
        bool reliable;
        bool is_response;
        bool loss_injectable;
        u8 data[ARQ_MAX_PAYLOAD];
    };

    /** 接收环里的一帧 */
    struct RxFrame
    {
        u8 src_mac[6];
        u16 len;
        u8 data[ARQ_FRAME_MAX_SIZE];
    };

    /** 对外可见的运行状态（m_state_mutex 保护） */
    struct State
    {
        bool peer_known;
        bool established;
        bool breaker_open;
        u8 window;
        u8 in_flight;
    };

    static void task_fn(void *arg);

    /** submit 与 submit_response 共用的入环路径 */
    Submit enqueue(const u8 *payload, u16 len, const u8 *dest_mac, const TxOptions &options, u16 cmd_seq,
                   bool is_response, u16 *seq_out);

    /** 任务主体：排空入向与待发，驱动核心，更新对外缓存，睡到下一个待办时刻 */
    void run_loop();
    void drain_rx(u32 now);
    void drain_tx(u32 now);
    void handle_frame(const u8 *src_mac, const u8 *data, size_t len, u32 now);
    void handle_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u32 now);
    void handle_reset(u32 peer_epoch, const ArqSettings &peer_settings, bool echo, u32 now);

    /** 按序交付连续区间 [first, last] 的载荷 */
    void deliver_range(u16 first, const u8 *first_payload, u16 first_len, u16 last);

    /** 会话握手、重放、重传与纯确认 */
    void poll_core(u32 now);
    u32 next_deadline_ms() const;

    /** 封一帧数据帧并发出去（任务上下文） */
    void emit_message(const u8 *dest_mac, u16 seq, const u8 *payload, u16 len, u8 flags, u16 ack, u16 sack, u8 window,
                      bool loss_injectable);
    void emit_ack(u16 ack, u16 sack, u8 window);
    void emit_reset(bool echo);
    void send_frame(const u8 *dest_mac, const u8 *payload, u16 len, u8 flags, u16 seq, u16 ack, u16 sack, u8 window,
                    bool reliable, bool loss_injectable);

    /** 对外缓存：状态、快照与参数副本一起换 */
    void publish_state();

    void record_failed(u16 seq);
    void reconfigure(const ArqSettings &settings);
    void reset_session(u32 now);

    /** 参数是否该采纳对端那份：代数大的赢，代数相同用 MAC 做确定性平局 */
    bool should_adopt_settings(const ArqSettings &peer) const;

    /** 标记参数待落盘（NVS 写入由主循环做） */
    void mark_settings_dirty();

    static void core_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before);
    static void core_event(void *raw, arq::Event event, u16 seq, u32 now_ms);

    ArqSettings m_settings = {};
    arq::Config m_config = {};
    size_t m_payload_cap = 0;
    u8 m_own_mac[6] = {};

    arq::Sender m_sender;
    arq::Receiver m_receiver;
    u8 m_tx_storage[static_cast<size_t>(arq::MAX_WINDOW) * MAX_PAYLOAD] = {};

    /** 接收侧的乱序载荷缓冲：窗口大于 1 时补齐落在游标之后的帧 */
    arq::ReorderStore m_rx_reorder;
    u8 m_rx_storage[static_cast<size_t>(arq::MAX_WINDOW) * MAX_PAYLOAD] = {};

    /** 出向封帧缓冲（任务独占，不放栈上：一整帧可达 1470B） */
    u8 m_frame[ARQ_FRAME_MAX_SIZE] = {};
    /** 接收环取出的帧（任务独占，同上） */
    RxFrame m_rx_frame = {};
    /** 投递用的帧缓冲：只在 ESP-NOW 回调里使用（回调单线程串行，与任务各用一份） */
    RxFrame m_rx_post = {};
    /** 待发环取出的消息（任务独占，同上） */
    TxMessage m_tx_msg = {};
    /** 提交时组帧用的消息缓冲（多上下文共享，由 m_submit_mutex 串行化） */
    TxMessage m_tx_post = {};
    /** 会话重置载荷（任务独占，组装好再交给 send_frame） */
    u8 m_reset_payload[ARQ_RESET_PAYLOAD_SIZE] = {};

    TaskHandle_t m_task = nullptr;
    QueueHandle_t m_rx_queue = nullptr;
    QueueHandle_t m_tx_reliable = nullptr;
    QueueHandle_t m_tx_unreliable = nullptr;
    /** 有新入向帧或新提交时唤醒任务；信号合并无害，任务醒来会把两个环都排空 */
    SemaphoreHandle_t m_wake = nullptr;
    /** 只保护对外缓存与设置副本，与 ARQ 状态无关 */
    mutable SemaphoreHandle_t m_state_mutex = nullptr;
    /** 保护提交用的组帧缓冲（提交可能来自多个任务上下文） */
    SemaphoreHandle_t m_submit_mutex = nullptr;

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

    /** 重复帧触发的重放：接收路径只记序号，真正的重发在 poll_core 里做 */
    bool m_replay_pending = false;
    u16 m_replay_cmd_seq = 0;

    /** 收到对端的重置请求需要回显 */
    bool m_reset_reply_pending = false;
    u32 m_reset_reply_due_ms = 0;

    /** 命令序号 → 响应序号（对端重传命令时据此重放响应） */
    u16 m_replay_map[arq::MAX_WINDOW] = {};
    bool m_replay_map_valid[arq::MAX_WINDOW] = {};

    /** 失败消息序号的小队列：核心事件回调只入队，上层用 take_failed 取走销账 */
    u16 m_failed_seqs[arq::MAX_WINDOW] = {};
    u8 m_failed_head = 0;
    u8 m_failed_count = 0;

    // 对外缓存（m_state_mutex 保护）
    State m_state = {};
    Snapshot m_snapshot = {};
    ArqSettings m_settings_view = {};
    bool m_settings_dirty = false;

    /** 主机下发的待应用参数：外部只登记，任务里换会话 */
    bool m_apply_pending = false;
    ArqSettings m_apply_settings = {};

    /** 外部登记的会话对端变更（m_state_mutex 保护，任务里落实） */
    bool m_peer_pending = false;
    bool m_peer_clear_pending = false;
    u8 m_peer_pending_mac[6] = {};

    /**
     * 可靠消息的序号预留：按提交顺序递增，与核心分配发送序号同源同序（提交严格
     * 按 FIFO，因此"第 n 条提交"就是"第 n 个序号"）。会话重置时归零并清空待发环。
     */
    u16 m_seq_counter = 0;

    /** 外部登记的"链路仍活着"（Pong 回显）：任务里清熔断 */
    volatile bool m_link_alive_pending = false;

    u32 m_replayed = 0;
    u32 m_replay_missed = 0;
    /** 环满丢弃计数：提交与接收回调可能来自多个上下文，统计口径允许偶发少记 */
    volatile u32 m_tx_dropped = 0;
    volatile u32 m_rx_dropped = 0;
};
} // namespace mlink

#endif // __MEGUMINLINK_ARQ_LINK_HPP__
