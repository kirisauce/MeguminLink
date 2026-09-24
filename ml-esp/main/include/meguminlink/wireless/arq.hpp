#pragma once
#ifndef __MEGUMINLINK_ARQ_HPP__
#define __MEGUMINLINK_ARQ_HPP__

#include "meguminlink/common.hpp"

#include <cstddef>

namespace mlink
{
/**
 * 单向可靠传输控制算法：滑动窗口 + 累积 ACK + 选择确认（SACK）+ 超时重传，
 * 与承载链路完全解耦。
 *
 * 本模块只做序号、窗口、定时器、确认与重传的判定，不认识 ESP-NOW，也不认识
 * CMSIS-DAP。三条解耦约定让它能原样搬到别的"一帧一条消息"的载体上，也能在
 * 宿主机上直接编译验证（见 ml-esp/tmp/arq_host_test.cc）：
 *
 * 1. 时间由外部传入：所有接口都吃调用方给的 now（毫秒），模块内部不取时钟；
 * 2. 缓冲由外部提供：窗口存储是一段调用方给的连续内存（slot_count × slot_cap）；
 * 3. 收发走回调：发送经 TransmitFn 交给调用方，事件经 EventFn 交给调用方。
 *
 * 一条"消息"就是一帧的载荷（本项目的 DAP 命令/响应都装得进单帧，不做分段）。
 * 序号空间为 u16，比较一律走 seq_before（模比较）：长时间运行必然回绕，用
 * 裸 < 比较会在回绕点把整窗判成过期。
 *
 * 调用方（适配层）的职责：读写帧的扩展头、把载荷拷进/拷出本模块的窗口存储、
 * 在 TransmitFn 回调里真正把帧送出去、以及收到重复帧时把缓存响应重新交付给
 * Sender::resend 重放。
 */
namespace arq
{
/**
 * 窗口上限：一次性分配的窗口槽数与序号位图宽度都按它来，运行时窗口参数只能
 * 在它以内调整。对齐 CMSIS-DAP 的 DAP_PACKET_COUNT 与 USB 命令 ringbuffer 深度。
 */
inline constexpr u8 MAX_WINDOW = 8;

/**
 * 选择确认位图的宽度（位）。位图的第 i 位（i ∈ [0, SACK_BITS)）表示
 * seq == 累积确认号 + 1 + i 这一帧已被单独收到：位图是相对确认号的，不随
 * 序号回绕失效。
 */
inline constexpr u16 SACK_BITS = 16;

/**
 * 两次重传之间的最小间隔。MAC 层的发送失败回调会在极短时间内连着来（一帧刚
 * 发出去就拿到失败回执），没有这道闸门的话几次失败会在几毫秒内烧光重传预算，
 * 而"MAC 确认丢了但帧其实到了"在本链路上是常态，那种情况下把消息判死是错的。
 * 突发失败因此被合并成一次提前重传，预算只按真实发出的帧数消耗。
 */
inline constexpr u16 MIN_RETX_INTERVAL_MS = 2;

/**
 * 回退系数上限：RTO 每次重传翻倍，最多翻到 2^8 倍后再由 rto_max_ms 截断。
 */
inline constexpr u8 MAX_BACKOFF_SHIFT = 8;

/**
 * 算法参数。默认值取自本链路的实测特征：无线单命令往返约 12ms（大头是目标侧
 * 的 SWD 执行），空口往返只有 2-3ms，所以 RTO 下限取 15ms；最坏重传序列
 * 15+30+60+120+240+250+250 ≈ 965ms，仍落在主机侧 1800ms 的命令超时之内。
 */
struct Config
{
    /** 发送窗口（可同时在途的消息数）；运行时可被对端通告的接收窗口压低 */
    u8 window = 8;

    /**
     * 接收端是否接受并标记乱序帧（SACK 相送）。
     *
     * 关：乱序帧判为 OutOfWindow，不标记也不 SACK，等发送端超时重传——正确但
     * 浪费空口，窗口为 1 时够用；开：乱序帧进 SACK 位图，调用方必须自己为重排
     * 缓冲准备载荷存储（见 ReorderStore），并在 Receiver::commit 返回的区间里
     * 把补齐的帧按序交付。
     *
     * 窗口大于 1 时必须打开：同时在途的多条消息一旦乱序就全被判超窗，只能靠
     * 重传逐条补齐，窗口带来的流水线收益会被重传风暴吃掉。
     */
    bool reorder = true;

    /**
     * RTO 下限。取值必须覆盖"对端执行整条命令"的时间：本链路的确认随响应捎带
     * 回来，一次往返里含执行时间，而命令长度差异极大（探测类命令执行不到 1ms，
     * 写满载荷的批量传输命令十几毫秒），估计值跟不上下限一低就会出现"RTT 超过
     * RTO → 重传 → 该条消息因 Karn 不再采样 → 估计值回不去"的自锁，之后每条大
     * 命令都白重传一次。
     */
    u16 rto_min_ms = 60;
    /** RTO 上限：指数退避到此为止 */
    u16 rto_max_ms = 250;
    /**
     * 还没有 RTT 样本时的初始 RTO。
     *
     * 不能取下限：本链路的确认是随响应捎带回来的，一次往返里含着对端执行整条
     * 命令的时间（实测约 13ms，批量传输命令更久），取下限会让会话开头几条命令
     * 必然白重传一次。第一个样本一到就切换到自适应估计，所以这个值只影响会话
     * 开头；它同时是重传预算的起点，取值过大会把最坏失败时刻顶到主机侧的命令
     * 超时之外（当前取值下最坏 1.42s，仍在 1.8s 之内）。
     */
    u16 initial_rto_ms = 60;
    /** 单条消息的重传次数上限（首传不计）；超限即判定失败并丢弃 */
    u8 max_retx = 6;
    /** 连续多少条消息重传耗尽后熔断（快速失败），0 = 不熔断 */
    u8 breaker_failures = 3;
    /** 收到需要确认的帧后，迟迟没有出向帧可捎带时，延迟多久补一个纯 ACK */
    u16 delayed_ack_ms = 2;
    /** 熔断期间每隔多久放行一条消息做试探，成功即恢复 */
    u16 breaker_probe_ms = 1000;
};

/** 提交结果 */
enum class SubmitResult : u8
{
    Ok,          /**< 已进入窗口并发出 */
    Invalid,     /**< 载荷为空或超过槽容量，未进入窗口 */
    WindowFull,  /**< 窗口已满（含对端通告的接收窗口为 0），未进入窗口 */
    BreakerOpen, /**< 熔断中且未到试探时刻，未进入窗口 */
};

/** 收到一帧后的判定结果 */
enum class RxResult : u8
{
    Delivered,   /**< 就是期待的那一帧：调用方存好载荷后调 commit 推进并交付 */
    Buffered,    /**< 窗口内的乱序帧，已记入 SACK 位图；调用方存好载荷，暂不交付 */
    Duplicate,   /**< 已收到过（重复帧）：调用方走重放或丢弃，不得重复交付 */
    OutOfWindow, /**< 超出接收窗口或接收端不支持重排：未标记，等对端重传 */
};

/** 算法内部事件，供调用方累加统计与打日志 */
enum class Event : u8
{
    Submitted,    /**< 一条新消息首次发出 */
    Retransmit,   /**< 一条已在窗内的消息被重传 */
    Acked,        /**< 一条消息被确认（累积确认或 SACK 单独确认） */
    Failed,       /**< 重传次数耗尽，消息被丢弃 */
    BreakerTrip,  /**< 连续失败达到阈值，进入熔断 */
    BreakerReset, /**< 熔断恢复 */
    Delivered,    /**< 按序交付一帧 */
    Buffered,     /**< 乱序帧记入 SACK 位图 */
    Duplicate,    /**< 收到已收到过的帧 */
};

/** 事件回调（ctx 由调用方在 set_callbacks 给出） */
using EventFn = void (*)(void *ctx, Event event, u16 seq, u32 now_ms);

/**
 * 发送回调：调用方负责把载荷封成帧送到链路上。
 * @param seq        本帧序号（调用方需原样写进扩展头）
 * @param payload    载荷（指向本模块窗口存储，回调期间有效）
 * @param len        载荷长度
 * @param retransmit true = 这是重传
 * @param gap_before true = 本序号之前的空洞已被本端放弃，接收端应把交付游标
 *                   直接跳到本序号（丢掉中间所有未交付的帧）。只在"本帧正是最老
 *                   未确认帧"时置位，因此语义是精确的：此前要么已确认、要么已
 *                   放弃，不存在仍在途的帧。接收端若不处理这个标记，一次放弃就
 *                   会在流里留下永久空洞，后续帧全被判成超窗而整条链路僵死。
 * @param meta        随消息携带的 1 字节元数据；重传与重放时原样带回（DAP 命令用
 *                    它承载主机侧 USB 传输通道，见 DAP_CHANNEL_*）
 */
using TransmitFn = void (*)(void *ctx, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before,
                            u8 meta);

/**
 * 序号模比较：seq 是否落在 ref 之前（累积确认覆盖的范围）。
 * u16 空间的"之前"按半空间判定，窗口远小于半个空间所以不会歧义。
 */
inline bool seq_before(u16 seq, u16 ref)
{
    return static_cast<i16>(static_cast<u16>(seq - ref)) < 0;
}

/** 序号距离：seq 相对 ref 的模差（seq >= ref 时为正，落在 ref 之前时为大值） */
inline u16 seq_diff(u16 seq, u16 ref)
{
    return static_cast<u16>(seq - ref);
}

/** SACK 命中判定：位图第 i 位表示 seq == ack + 1 + i */
inline bool sack_test(u16 bitmap, u16 ack, u16 seq)
{
    const u16 rel = seq_diff(seq, ack);
    return rel >= 1 && rel <= SACK_BITS && ((bitmap >> (rel - 1)) & 1u) != 0;
}

/** SACK 置位：与 sack_test 同一套编码 */
inline u16 sack_set(u16 bitmap, u16 ack, u16 seq)
{
    const u16 rel = seq_diff(seq, ack);
    if (rel < 1 || rel > SACK_BITS)
    {
        return bitmap;
    }
    return static_cast<u16>(bitmap | (1u << (rel - 1)));
}

/**
 * 往返时延估计（简化 Jacobson/Karels）：
 *   RTTVAR = 3/4 · RTTVAR + 1/4 · |SRTT − 样本|
 *   SRTT   = 7/8 · SRTT   + 1/8 · 样本
 *   RTO    = clamp(SRTT + 4·RTTVAR, 下限, 上限)
 * 重传过的消息不参与采样（Karn 算法）：那种样本分不清是首传还是重传的往返，
 * 混进来会把估计值越拉越偏。
 */
class RttEstimator
{
  public:
    void init(u16 min_ms, u16 max_ms, u16 initial_ms);

    /** 喂一个往返样本（毫秒） */
    void update(u32 sample_ms);

    /** 基础 RTO：还没有样本时用初始值，之后是 clamp(SRTT + 4·RTTVAR, 下限, 上限) */
    u32 base_ms() const;

    /** 带退避的 RTO：基础值左移 shift 位后按上限截断（shift 由每条消息各自持有） */
    u32 rto_ms(u8 shift) const;

    u32 srtt_ms() const
    {
        return m_srtt_ms;
    }

    u32 rttvar_ms() const
    {
        return m_rttvar_ms;
    }

    bool has_sample() const
    {
        return m_have_sample;
    }

  private:
    u16 m_min_ms = 15;
    u16 m_max_ms = 250;
    u16 m_initial_ms = 60;
    u32 m_srtt_ms = 0;
    u32 m_rttvar_ms = 0;
    bool m_have_sample = false;
};

/**
 * 连续失败熔断：连续 breaker_failures 条消息重传耗尽后判定链路不可用，之后的
 * 提交直接快速失败，避免对端已经挂掉时每条命令都硬等满重传预算。熔断期间每隔
 * breaker_probe_ms 放行一条消息做试探，任一条消息确认成功即恢复。
 */
class Breaker
{
  public:
    void init(u8 threshold, u16 probe_ms);

    /** 到点即放行一条试探并顺延；未熔断时恒为 true */
    bool allow(u32 now_ms);

    /**
     * 现在提交是否会被放行：只查询、不顺延试探时刻（allow 的只读版本）。
     * 未熔断恒为 true，熔断时只有到试探时刻才为 true。
     */
    bool submit_allowed(u32 now_ms) const;

    /** 一条消息重传耗尽；返回 true 表示本次调用触发了熔断 */
    bool on_failure(u32 now_ms);

    /** 任一消息确认成功：连续失败清零；返回 true 表示刚从熔断恢复 */
    bool on_success();

    bool tripped() const
    {
        return m_tripped;
    }

    u32 trips() const
    {
        return m_trips;
    }

    u8 consecutive_failures() const
    {
        return m_consecutive;
    }

  private:
    u8 m_threshold = 3;
    u16 m_probe_ms = 1000;
    u8 m_consecutive = 0;
    bool m_tripped = false;
    u32 m_trips = 0;
    u32 m_probe_at_ms = 0;
};

/** 发送方向的运行快照（统计上报与日志用） */
struct SenderSnapshot
{
    u32 tx_frames;     /**< 首次发出的消息数 */
    u32 retx_frames;   /**< 重传次数 */
    u32 acked_frames;  /**< 被确认的消息数 */
    u32 failed_frames; /**< 重传耗尽被丢弃的消息数 */
    u32 breaker_trips; /**< 熔断触发次数 */
    u32 srtt_ms;       /**< 平滑往返时延 */
    u32 rttvar_ms;     /**< 往返时延偏差 */
    u32 rto_ms;        /**< 当前基础 RTO */
    u8 in_flight;      /**< 已发出未确认的消息数 */
    u8 window;         /**< 当前生效窗口（配置值与对端通告取小） */
    bool breaker_open; /**< 是否处于熔断 */
};

/** 接收方向的运行快照 */
struct ReceiverSnapshot
{
    u32 delivered;     /**< 按序交付的帧数 */
    u32 buffered;      /**< 记入 SACK 位图的乱序帧数 */
    u32 duplicate;     /**< 重复帧数 */
    u32 out_of_window; /**< 超窗/不支持重排而拒绝的帧数 */
    u32 skipped;       /**< 因对端放弃空洞而被跳过的序号数 */
    u16 ack;           /**< 当前累积确认号（下一个期待收到的序号） */
    u16 sack;          /**< 当前选择确认位图 */
    u8 window;         /**< 当前通告的接收窗口 */
};

/**
 * 发送方向的控制块：序号分配、窗口占用、确认处理、超时重传、熔断。
 *
 * 窗口存储是一段调用方给的连续内存，第 i 个槽的载荷在 storage + i · slot_cap，
 * 槽的归属由 seq % slot_count 决定。只要 slot_count ≥ 生效窗口，在途消息就不会
 * 互相覆盖；一次性按 MAX_WINDOW 分配可让运行时窗口调整不需要重新规划内存。
 */
class Sender
{
  public:
    /**
     * @param config      算法参数（构造后不变；改参数请重新 init 并重建状态）
     * @param storage     窗口存储（容量 ≥ slot_count × slot_cap）
     * @param slot_cap    单个槽的载荷容量
     * @param slot_count  槽数（1..MAX_WINDOW）
     */
    void init(const Config &config, u8 *storage, size_t slot_cap, size_t slot_count);

    /**
     * 清空窗口、序号与定时器，把首帧序号复位到 0。
     *
     * 序号空间是会话内的相对量，两端必须从同一个基准起步，否则接收端会把对端
     * 重启后的新序号当成重复帧丢掉，而它回报的累积确认号又会把这些帧"确认"掉
     * ——消息静默消失。链路上每次建立会话时两端各调用一次即可对齐，不需要额外
     * 握手；累计统计不受影响。
     */
    void reset();

    void set_callbacks(void *ctx, TransmitFn transmit, EventFn event);

    /**
     * 提交一条消息：分配序号、拷进窗口、立即首次发送。
     * @param seq_out 可选，成功时回填本条消息的序号
     */
    SubmitResult submit(const u8 *payload, u16 len, u8 meta, u32 now_ms, u16 *seq_out = nullptr);

    /**
     * 重放一条仍在窗内的消息（接收端再次索要同一响应时用）。
     * @return false = 该序号已不在窗内（已被确认或已被判失败）
     */
    bool resend(u16 seq, u32 now_ms);

    /**
     * 收到对端帧的扩展头：累积确认 + 选择确认 + 对端接收窗口通告。
     * @return true = 本次确认推进了至少一条消息（调用方据此清 delayed ACK 与统计）
     */
    bool on_ack(u16 ack, u16 sack_bitmap, u8 peer_window, u32 now_ms);

    /**
     * 承载链路报"这一帧没发出去"（如 ESP-NOW 的发送失败回调）。
     * 只是一条提示：真正重传在 poll 里做，且受 MIN_RETX_INTERVAL_MS 合并，
     * 以免一串失败回执瞬间烧光重传预算。
     */
    void on_mac_failure(u32 now_ms);

    /** 驱动定时器：提前重传、RTO 到期重传、退避、超时判死。 */
    void poll(u32 now_ms);

    /**
     * 下一个需要被 poll 的时刻（毫秒绝对值）；0 = 没有待办。
     * 调用方据此设置等待超时，空闲时不必轮询。
     */
    u32 next_deadline_ms() const;

    bool idle() const
    {
        return m_tx_base == m_tx_next;
    }

    u8 in_flight() const
    {
        return static_cast<u8>(seq_diff(m_tx_next, m_tx_base));
    }

    bool broken() const
    {
        return m_breaker.tripped();
    }

    /** 当前生效窗口 = min(配置窗口, 对端通告的接收窗口) */
    u8 window() const;

    SenderSnapshot snapshot() const;

    /**
     * 现在提交是否可能产生流量：窗口有空位，且熔断时只在试探时刻为真。
     *
     * 与 submit() 里的 allow() 判断同源。调用方（USB 侧的背压）据此决定取不取命令：
     * 熔断时若一律返回 false，就永远不会有试探提交，熔断再也恢复不了。
     */
    bool can_submit(u32 now_ms) const;

    /**
     * 链路被证明可用（收到对端任何帧，例如探针 Ping 的 Pong）：清掉熔断与连续失败计数。
     * @return true = 刚从熔断恢复
     */
    bool notify_link_alive();

  private:
    /** 一条在途消息的状态 */
    struct Slot
    {
        u16 seq;
        u16 len;
        u8 meta;          /**< 随消息携带的 1 字节元数据，重传时原样带回 */
        u8 retx;          /**< 已重传次数（首传不计） */
        u8 backoff_shift; /**< 本条消息的退避指数 */
        bool used;
        bool ever_retx; /**< Karn：重传过的消息不参与 RTT 采样 */
        u32 first_tx_ms;
        u32 deadline_ms;      /**< 下一次 RTO 到期时刻 */
        u32 earliest_retx_ms; /**< 允许下一次重传的最早时刻（合并失败突发） */
    };

    Slot &slot_of(u16 seq);
    const Slot &slot_of(u16 seq) const;
    u8 *payload_of(u16 seq);
    const u8 *payload_of(u16 seq) const;
    void release(u16 seq);
    void retransmit(u16 seq, u32 now_ms);
    void fail(u16 seq, u32 now_ms);
    void emit(Event event, u16 seq, u32 now_ms) const;

    Config m_config;

    u8 *m_storage = nullptr;
    size_t m_slot_cap = 0;
    size_t m_slot_count = 0;

    Slot m_slots[MAX_WINDOW] = {};

    u16 m_tx_base = 0; /**< 最老的未确认序号 */
    u16 m_tx_next = 0; /**< 下一个待分配的序号 */

    /** MAC 发送失败提示：置位后由 poll 在最早允许时刻合并成一次提前重传 */
    bool m_mac_failed = false;

    /**
     * 是否发生过"消息重传耗尽被放弃"。置位后，落在窗口最老位置的出向帧会带上
     * gap_before 标记，让接收端把被放弃的空洞跳过去；一旦接收端跳过去，标记就
     * 自然失效（此时标记帧的序号与对端游标相同，跳转退化成空操作）。
     */
    bool m_abandoned = false;

    /** 对端在最近一帧里通告的接收窗口（初值乐观取满，收到通告后按实值收敛） */
    u8 m_peer_window = MAX_WINDOW;

    RttEstimator m_rtt;
    Breaker m_breaker;

    void *m_ctx = nullptr;
    TransmitFn m_transmit = nullptr;
    EventFn m_event = nullptr;

    u32 m_tx_frames = 0;
    u32 m_retx_frames = 0;
    u32 m_acked_frames = 0;
    u32 m_failed_frames = 0;
};

/**
 * 接收侧的乱序载荷存储：与 Sender 的窗口存储对称，内存由调用方提供，槽位由
 * seq % slot_count 决定。
 *
 * 它只做"存、取、失效"三件事，判定仍归 Receiver：调用方必须先 on_frame 拿到
 * 判定、再决定存不存 —— 重复帧的槽位可能与某个还没交付的乱序帧撞车，顺序反了
 * 会覆盖掉有效载荷。交付由调用方按 Receiver::commit 返回的区间逐帧取出。
 *
 * 只有 reorder 打开且窗口大于 1 时才用得上：窗口为 1 时乱序帧被判 OutOfWindow、
 * 由对端超时重传补齐，不经过乱序缓冲。
 */
class ReorderStore
{
  public:
    /**
     * @param storage    槽存储（容量 ≥ slot_count × slot_cap）
     * @param slot_cap   单个槽的载荷容量
     * @param slot_count 槽数（1..MAX_WINDOW）；应不小于生效窗口，否则同时在途的
     *                   乱序帧会落到同一个槽上互相覆盖
     */
    void init(u8 *storage, size_t slot_cap, size_t slot_count);

    /** 存一帧乱序载荷（仅在 on_frame 返回 Buffered 后调用）；超长按 slot_cap 截断 */
    void store(u16 seq, const u8 *payload, u16 len, u8 meta);

    /**
     * 取一帧载荷。
     * @param len  可选，回填该帧的载荷长度
     * @param meta 可选，回填该帧随消息携带的 1 字节元数据
     * @return 载荷指针；该序号没有有效载荷时返回 nullptr
     */
    const u8 *get(u16 seq, u16 *len, u8 *meta) const;

    /** 释放一帧（交付完成或确认丢弃后调用） */
    void release(u16 seq);

    /** 清空全部槽位（会话重置、参数变更时调用） */
    void reset();

  private:
    size_t slot_of(u16 seq) const;

    u8 *m_storage = nullptr;
    size_t m_slot_cap = 0;
    size_t m_slot_count = 1;
    u16 m_len[MAX_WINDOW] = {};
    u8 m_meta[MAX_WINDOW] = {};
    bool m_valid[MAX_WINDOW] = {};
};

/**
 * 接收方向的控制块：按序交付、乱序标记（SACK）、重复识别、接收窗口通告。
 *
 * 它不持有载荷：载荷由调用方按序号存放在自己的缓冲里。调用方对每一帧的处理
 * 顺序是"先 on_frame 拿判定，再决定存不存"，因为重复帧的槽位可能与某个乱序帧
 * 撞车（同一序号模窗口），先存会覆盖掉还没交付的数据。
 */
class Receiver
{
  public:
    void init(const Config &config);

    /**
     * 把交付游标与 SACK 位图复位成"期待序号 initial_seq"。
     *
     * 基准必须由调用方给定，不能拿第一个到达的帧去猜：会话刚开始时若发生丢帧
     * 或乱序，第一个到达的不会是序号初值，猜错就会把还没收到的帧判成重复帧丢
     * 弃，同时用累积确认号把它"确认"掉。两端在链路建立时各自 reset 一次即可
     * 对齐。
     */
    void reset(u16 initial_seq);

    void set_callbacks(void *ctx, EventFn event);

    /**
     * 判定一帧。
     *
     * @param gap_before 对端声明"本序号之前的空洞已被放弃"：交付游标直接跳到本
     *                   序号，中间未交付的帧按丢失处理。这是流从"消息重传耗尽"
     *                   里恢复的唯一手段。
     *
     * 返回 Delivered 时调用方必须接着调 commit 推进交付游标；返回 Buffered 时
     * 调用方把载荷存进自己的重排缓冲，等 commit 的区间把它补出来。
     */
    RxResult on_frame(u16 seq, bool gap_before, u32 now_ms);

    /**
     * 推进交付游标（仅在 on_frame 返回 Delivered 后调用）。
     * @return 本次连续交付的最后一帧序号；调用方按序交付 [seq, 返回值] 的全部
     *         载荷（乱序缓冲补齐时返回值会大于 seq）
     */
    u16 commit(u16 seq, u32 now_ms);

    /** 累积确认号 = 下一个期待收到的序号 */
    u16 ack() const
    {
        return m_rx_next;
    }

    /** 选择确认位图（相对 ack） */
    u16 sack_bitmap() const
    {
        return m_received;
    }

    /** 当前通告的接收窗口（本端还能再收多少帧乱序数据） */
    u8 window() const;

    /** 是否有待确认的收帧（调用方据此决定是否/何时补纯 ACK） */
    bool ack_pending() const
    {
        return m_ack_pending;
    }

    /** 确认已随某帧发出（含纯 ACK） */
    void clear_ack_pending()
    {
        m_ack_pending = false;
    }

    ReceiverSnapshot snapshot() const;

  private:
    Config m_config;

    u16 m_rx_next = 0;  /**< 交付游标 = 累积确认号 */
    u16 m_received = 0; /**< 乱序（含已缓冲）位图：位 i 表示 seq == m_rx_next + 1 + i */
    bool m_ack_pending = false;

    void *m_ctx = nullptr;
    EventFn m_event = nullptr;

    u32 m_delivered = 0;
    u32 m_buffered = 0;
    u32 m_duplicate = 0;
    u32 m_out_of_window = 0;
    u32 m_skipped = 0; /**< 因对端放弃空洞而被跳过的序号数 */

    void emit(Event event, u16 seq, u32 now_ms) const;
};
} // namespace arq
} // namespace mlink

#endif // __MEGUMINLINK_ARQ_HPP__
