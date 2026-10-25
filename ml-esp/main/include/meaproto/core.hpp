#pragma once
#ifndef __MEAPROTO_CORE_HPP__
#define __MEAPROTO_CORE_HPP__

#include "meaproto/config.hpp"
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
    static usize storage_bytes(std::span<const ChannelSpec> channels) noexcept;

    /** 提交一条消息：NotConnected / InvalidChannel / TooLarge / Busy / Ok */
    SendResult send(ChannelId channel, std::span<const u8> data) noexcept;

    /** 该通道发送队列是否还有空位（背压用） */
    bool can_send(ChannelId channel) const noexcept;

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

    Stats snapshot() const noexcept;

  private:
    /** 一条通道的发送队列；存储块由构造函数从注入的缓冲里切分 */
    struct Channel
    {
        ChannelSpec spec{};
        u8 *queue = nullptr; /**< queue_depth × 条目步长 */
        u16 head = 0;
        u16 tail = 0;
        u16 count = 0;
    };

    /** 条目头：消息长度 */
    static constexpr usize ENTRY_HEADER = sizeof(u16);

    /** 一条消息在队列里的步长（长度前缀 + 载荷，按 u16 对齐） */
    static usize entry_stride(const ChannelSpec &spec) noexcept;

    Channel *find_channel(ChannelId channel) noexcept;
    const Channel *find_channel(ChannelId channel) const noexcept;

    /** 编码并发出一个控制帧 */
    bool send_control(u32 now, ControlType type, ControlStatus status, u32 peer_epoch, u16 token) noexcept;

    void handle_control(u32 now, const ControlFrame &frame) noexcept;
    void handle_connect_request(u32 now, const ControlFrame &frame) noexcept;
    void handle_connect_accept(const ControlFrame &frame) noexcept;
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

    // ---- 运行期 ----
    u32 m_last_tx_ms = 0; /**< 最近一次成功发送的时刻，保活用 */
    Stats m_stats;        /**< 计数器，snapshot() 取副本 */
};
} // namespace meaproto

#endif // __MEAPROTO_CORE_HPP__
