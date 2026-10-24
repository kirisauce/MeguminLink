#include "meaproto/core.hpp"

#include "meaproto/hash.hpp"

namespace meaproto
{
namespace
{
/** 没有定时器时的兜底唤醒间隔 */
constexpr u32 IDLE_WAKE_MS = 1000;
} // namespace

Core::Core(std::span<const ChannelSpec> channels, const Config &config, u16 mtu, Sink &sink,
           const CoreHooks &hooks) noexcept
    : m_config(config), m_mtu(mtu), m_config_hash(config_hash(channels)), m_sink(sink), m_hooks(hooks)
{
}

void Core::open(u32 now) noexcept
{
    if (m_state != State::Idle)
    {
        return;
    }

    m_own_epoch = next_epoch();
    m_peer_epoch = 0;
    m_state = State::Connecting;
    (void)send_control(now, ControlType::ConnectRequest, ControlStatus::Ok, 0, 0);
}

void Core::close(u32 now) noexcept
{
    if (m_state == State::Idle)
    {
        return;
    }

    (void)send_control(now, ControlType::Disconnect, ControlStatus::Ok, m_peer_epoch, 0);
    enter_idle(DisconnectReason::Requested);
}

u32 Core::step(u32 now) noexcept
{
    return now + IDLE_WAKE_MS;
}

Core::State Core::state() const noexcept
{
    return m_state;
}

bool Core::connected() const noexcept
{
    return m_state == State::Connected;
}

bool Core::connecting() const noexcept
{
    return m_state == State::Connecting;
}

Stats Core::snapshot() const noexcept
{
    Stats stats = m_stats;
    stats.connected = connected();
    stats.peer_epoch = static_cast<u16>(m_peer_epoch);
    return stats;
}

bool Core::send_control(u32 now, ControlType type, ControlStatus status, u32 peer_epoch, u16 token) noexcept
{
    ControlFrame frame;
    frame.type = type;
    frame.status = status;
    frame.peer_epoch = peer_epoch;
    frame.own_epoch = m_own_epoch;
    frame.mtu = m_mtu;
    frame.config_hash = m_config_hash;
    frame.token = token;

    u8 buffer[CONTROL_FRAME_SIZE] = {};
    usize written = 0;
    if (encode(frame, std::span<u8>{buffer, sizeof(buffer)}, written) != WireError::Ok)
    {
        return false;
    }
    if (m_hooks.send == nullptr || !m_hooks.send(m_hooks.ctx, std::span<const u8>{buffer, written}))
    {
        return false;
    }

    m_last_tx_ms = now;
    return true;
}

void Core::enter_idle(DisconnectReason reason) noexcept
{
    const bool was_active = m_state != State::Idle;
    m_state = State::Idle;
    m_own_epoch = 0;
    m_peer_epoch = 0;
    if (was_active)
    {
        m_sink.on_disconnected(reason);
    }
}

u32 Core::next_epoch() noexcept
{
    const u32 epoch = m_hooks.random != nullptr ? m_hooks.random(m_hooks.ctx) : 0;
    return epoch == 0 ? 1 : epoch;
}
} // namespace meaproto
