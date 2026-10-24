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

    /** 参数顺序：本端协议参数（通道表、配置、MTU）→ 应用接口 → 平台回调 */
    Core(std::span<const ChannelSpec> channels, const Config &config, u16 mtu, Sink &sink,
         const CoreHooks &hooks) noexcept;

    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;

    /** 发起握手；非 Idle 时幂等 */
    void open(u32 now) noexcept;

    /** 发出 Disconnect 并回到 Idle；Idle 时无操作 */
    void close(u32 now) noexcept;

    /** 一次推进：定时器与待发帧，返回下次需要唤醒的时刻 */
    u32 step(u32 now) noexcept;

    State state() const noexcept;
    bool connected() const noexcept;
    bool connecting() const noexcept;
    Stats snapshot() const noexcept;

  private:
    /** 编码并发出一个控制帧 */
    bool send_control(u32 now, ControlType type, ControlStatus status, u32 peer_epoch, u16 token) noexcept;

    /** 回到 Idle，清理会话并回调 Sink */
    void enter_idle(DisconnectReason reason) noexcept;

    /** 取一个非零会话标识 */
    u32 next_epoch() noexcept;

    Config m_config;
    u16 m_mtu;
    u16 m_config_hash;
    Sink &m_sink;
    CoreHooks m_hooks;
    State m_state = State::Idle;
    u32 m_own_epoch = 0;
    u32 m_peer_epoch = 0;
    u32 m_last_tx_ms = 0;
    Stats m_stats;
};
} // namespace meaproto

#endif // __MEAPROTO_CORE_HPP__
