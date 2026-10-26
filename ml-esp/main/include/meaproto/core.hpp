#pragma once
#ifndef __MEAPROTO_CORE_HPP__
#define __MEAPROTO_CORE_HPP__

#include "meaproto/config.hpp"
#include "meaproto/reassembly.hpp"
#include "meaproto/sink.hpp"
#include "meaproto/types.hpp"
#include "meaproto/wire.hpp"

#include <cstddef>
#include <span>

namespace meaproto
{
/**
 * Core 的外部依赖，两者共用同一个上下文。
 *
 * send 把一帧交给 Transport，返回 false 表示底层暂时发不出去、下一拍重试；
 * random 供握手取会话标识（固件用 esp_random，宿主测试用确定性序列）。
 */
struct CoreHooks
{
    void *ctx = nullptr;
    bool (*send)(void *ctx, std::span<const u8> data) = nullptr;
    u32 (*random)(void *ctx) = nullptr;
};

/**
 * 连接状态机与本端发送出口。
 *
 * 不依赖 RTOS、不加锁、不写日志：时间由 step() 注入，发送与事件走构造时给的
 * 回调，因此可以在宿主机上直接驱动。
 */
class Core
{
  public:
    enum class State : u8
    {
        Idle,
        Connecting,
        Connected,
    };

    /** 参数顺序：本端协议参数（通道表、配置、MTU）→ 应用接口 → 平台回调 → 通道存储 */
    Core(std::span<const ChannelSpec> channels, const Config &config, u16 mtu, Sink &sink, const CoreHooks &hooks,
         std::span<u8> storage) noexcept;

    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;

    /** 发起握手；非 Idle，或距上一次失败不足 connect_retry_ms 时忽略 */
    void open(u32 now) noexcept;

    /** 发出 Disconnect 并回到 Idle；Idle 时无操作 */
    void close(u32 now) noexcept;

    /** 处理一帧接收字节：控制帧进握手状态机，其余整帧丢弃 */
    void receive(u32 now, std::span<const u8> frame) noexcept;

    /** 一次推进：握手重发与超时，返回下次需要唤醒的时刻 */
    u32 step(u32 now) noexcept;

    /**
     * 通道存储需求（字节）：按通道表的队列深度与载荷上限算出，由调用方分配后交给
     * 构造函数。协议层因此不关心存储来自内部 RAM 还是 PSRAM。
     */
    static usize storage_bytes(std::span<const ChannelSpec> channels, const Config &config, u16 mtu) noexcept;

    /** 提交一条消息：NotConnected / InvalidChannel / TooLarge / Busy / Ok */
    SendResult send(ChannelId channel, std::span<const u8> data) noexcept;

    /** 该通道发送队列是否还有空位（背压用） */
    bool can_send(ChannelId channel) const noexcept;

    /**
     * 切换通道的可靠性策略；只在 Connected 生效，切换会清空该通道的队列、窗口与
     * 接收侧状态，序号从 0 重新开始。两端必须同时切换。
     */
    bool set_reliability(ChannelId channel, Reliability reliability) noexcept;

    State state() const noexcept;
    bool connected() const noexcept;
    bool connecting() const noexcept;

    /** 握手协商后的单帧上限 */
    u16 mtu() const noexcept;

    /** 本端会话标识 */
    u32 own_epoch() const noexcept;

    /** 对端会话标识 */
    u32 peer_epoch() const noexcept;

    /** 通道表摘要 */
    u16 config_hash() const noexcept;

    /** 发一个 Ping；未连接时返回 false */
    bool ping(u32 now) noexcept;

    /** 构造时注入的运行参数副本 */
    const Config &config() const noexcept;

    /**
     * 运行期改时序类参数（RTO、保活、超时、配额、延迟确认）。
     *
     * 尺寸类字段（窗口、接收队列、重组槽、消息上限）在构造时已按存储切分，
     * 这里忽略；取值非法时整体不生效并返回 false。
     */
    bool apply_config(const Config &config) noexcept;

    Stats snapshot() const noexcept;

  private:
    /** 发送窗口槽：保存整帧字节，超时原样重发 */
    struct WindowSlot
    {
        bool used = false;
        u16 seq = 0;
        u16 len = 0;
        u8 retx = 0;
        u32 sent_ms = 0;
        u32 deadline_ms = 0;
    };

    /** 一条通道：队列、窗口与序号；存储块由构造函数从注入的缓冲里切分 */
    struct Channel
    {
        ChannelSpec spec{};
        u8 *queue = nullptr; /**< queue_depth × 条目步长 */
        u16 head = 0;
        u16 tail = 0;
        u16 count = 0;
        u8 *window = nullptr; /**< window_count × 帧槽步长 */
        u16 window_count = 0;
        u16 used_slots = 0;
        u16 tx_seq = 0;
        u16 next_ordering = 0; /**< 下一条消息的 ordering，send() 时分配 */
        u16 next_split_id = 1; /**< 下一条分片消息的 split_id（0 表示未分片） */
        WindowSlot slots[MAX_WINDOW];
        u8 *reorder = nullptr; /**< reorder_count × 重排步长 */
        u16 reorder_count = 0;
        u16 reorder_used = 0;
        u16 rx_expected = 0; /**< 下一个期望的数据报序号 */
        u32 rx_bits = 0;     /**< expected+1+i 已收到 */
        u16 rx_ordering = 0; /**< 下一个期望交付的消息序号 */
        bool ack_pending = false;
        u32 ack_deadline_ms = 0;

        /** 清空会话状态（队列、窗口、序号、重排与确认），保留 spec 与存储指针 */
        void reset() noexcept;
    };

    /** 队列条目头：一条消息可能拆成多片，每片占一个条目 */
    struct ChunkHeader
    {
        u16 len = 0;         /**< 片载荷长度 */
        u16 ordering = 0;    /**< 可靠通道内递增；同一条消息的各片相同 */
        u16 split_id = 0;    /**< 0 表示未分片 */
        u16 split_index = 0; /**< 片下标，0 起始 */
        u16 split_count = 1; /**< 总片数，1 表示未分片 */
    };

    static constexpr usize ENTRY_HEADER = sizeof(ChunkHeader);

    /** 窗口帧槽的长度前缀 */
    static constexpr usize SLOT_HEADER = sizeof(u16);

    /** 一个队列条目的步长（分片头 + 片载荷，按 u16 对齐） */
    static usize entry_stride(const ChannelSpec &spec, const Config &config, u16 mtu) noexcept;

    /** 一个窗口帧槽的步长（长度前缀 + 整帧字节，按 u16 对齐） */
    static usize frame_stride(u16 mtu) noexcept;

    /** 一个重排槽的步长（ordering + 长度 + 载荷，按 u16 对齐） */
    static usize reorder_stride(const ChannelSpec &spec, const Config &config) noexcept;

    /** 有效重排深度：缺省取队列深度，封顶 ACK 位图宽度 */
    static u16 reorder_capacity(const ChannelSpec &spec) noexcept;

    /** 单帧可承载的消息载荷上限（消息头按可靠性取 3B / 5B） */
    static u16 frame_payload_capacity(Reliability reliability, u16 mtu) noexcept;

    /** 单条消息上限：min(max_payload, max_message_size) */
    static usize message_limit(const ChannelSpec &spec, const Config &config) noexcept;

    Channel *find_channel(ChannelId channel) noexcept;
    const Channel *find_channel(ChannelId channel) const noexcept;

    /** 队列非空且窗口有空位时组帧发出，成功则出队、占窗 */
    bool transmit(Channel &channel, u32 now) noexcept;

    /** 重发到期的窗口槽，返回是否有动作 */
    bool retransmit(u32 now) noexcept;

    /** 每拍的数据报调度：先重传，再按优先级与配额发新帧 */
    void advance_datagrams(u32 now) noexcept;

    /** 收到的数据报：释放发送窗口、去重、排序交付并安排延迟确认 */
    void handle_datagram(u32 now, std::span<const u8> frame) noexcept;

    /** 按 ack_seq / ack_bits 释放本通道窗口槽，顺带采样 RTT */
    void handle_ack(Channel &channel, u16 ack_seq, u32 ack_bits, u32 now) noexcept;

    /** 一次 RTT 样本进指数平滑估计器 */
    void sample_rtt(u32 sample) noexcept;

    /** 交付一条消息；可靠通道按 ordering 排队，重排缓冲满时置 overflow */
    void deliver_message(Channel &channel, u16 ordering, const u8 *data, u16 len, bool &overflow) noexcept;

    /** 把重排缓冲里连续的消息按序交付 */
    void drain_reorder(Channel &channel) noexcept;

    /** 发一个只带 ACK 字段的数据报 */
    bool send_ack(Channel &channel, u32 now) noexcept;

    /** 到期的延迟确认 */
    void flush_acks(u32 now) noexcept;

    /** 重组器收齐消息后的交付回调 */
    static bool deliver_hook(void *ctx, ChannelId channel, u16 ordering, const u8 *data, usize len) noexcept;

    /** 重传退避：rto_initial << retx，封顶 rto_max */
    u32 rto_backoff(u8 retx) const noexcept;

    /** 编码并发出一个控制帧 */
    bool send_control(u32 now, ControlType type, ControlStatus status, u32 peer_epoch, u16 token) noexcept;

    void handle_control(u32 now, const ControlFrame &frame) noexcept;
    void handle_connect_request(u32 now, const ControlFrame &frame) noexcept;
    void handle_connect_accept(u32 now, const ControlFrame &frame) noexcept;
    void handle_connect_reject(u32 now, const ControlFrame &frame) noexcept;
    void handle_disconnect(u32 now, const ControlFrame &frame) noexcept;

    /** 版本不符的 ConnectRequest：按 v1 布局读不出完整帧，只认类型字节 */
    bool reject_version(u32 now, std::span<const u8> frame) noexcept;

    /** 回到 Idle，清理会话并回调 Sink */
    void enter_idle(DisconnectReason reason) noexcept;

    /** 下一次需要唤醒的时刻 */
    u32 next_wake(u32 now) const noexcept;

    /** 握手重发间隔（0 视为 1，避免同一拍里反复重发） */
    u32 retry_ms() const noexcept;

    /** 取一个非零会话标识 */
    u32 next_epoch() noexcept;

    // ---- 外部依赖 ----
    Sink &m_sink;
    CoreHooks m_hooks;

    // ---- 本端参数（构造时确定）----
    Config m_config;
    u16 m_mtu;         /**< 本端单帧上限 */
    u16 m_config_hash; /**< 通道表摘要，握手时比对 */

    // ---- 会话 ----
    State m_state = State::Idle;
    u32 m_own_epoch = 0;  /**< 本端会话标识 */
    u32 m_peer_epoch = 0; /**< 对端会话标识 */
    u16 m_peer_mtu = 0;   /**< 协商结果：min(本端, 对端) */

    // ---- 握手进度 ----
    u32 m_handshake_deadline_ms = 0; /**< 下一次重发 ConnectRequest 的时刻 */
    u32 m_handshake_gate_ms = 0;     /**< 早于该时刻的 open() 被忽略 */
    u8 m_handshake_retries = 0;      /**< 已重发次数 */

    // ---- 通道 ----
    Channel m_channels[MAX_CHANNELS];
    u8 m_channel_count = 0;
    /** 组帧暂存，容量为本端 MTU */
    u8 *m_frame = nullptr;
    u16 m_frame_cap = 0;

    // ---- 分片重组 ----
    Reassembler m_reassembly;

    // ---- 运行期 ----
    u32 m_last_tx_ms = 0;     /**< 最近一次成功发出帧的时刻，保活用 */
    u32 m_last_rx_ms = 0;     /**< 最近一次收到合法帧的时刻，超时用 */
    u16 m_ping_token = 1;     /**< 下一个 Ping 的 token，0 保留 */
    u16 m_ping_in_flight = 0; /**< 在途 Ping 的 token，0 表示没有 */
    u32 m_ping_sent_ms = 0;
    u32 m_rtt_ms = 0; /**< 平滑后的往返时延 */
    u32 m_rto_ms = 0; /**< 由 RTT 推出的重传基准 */
    Stats m_stats;    /**< 计数器，snapshot() 取副本 */
};
} // namespace meaproto

#endif // __MEAPROTO_CORE_HPP__
