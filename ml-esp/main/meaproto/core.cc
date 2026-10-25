#include "meaproto/core.hpp"

#include "meaproto/hash.hpp"

#include <cstring>

namespace meaproto
{
namespace
{
/** 没有定时器时的兜底唤醒间隔 */
constexpr u32 IDLE_WAKE_MS = 1000;

/** u32 计时是回绕安全的比较：now 是否还没到 deadline */
bool time_before(u32 now, u32 deadline) noexcept
{
    return static_cast<i32>(now - deadline) < 0;
}
} // namespace

Core::Core(std::span<const ChannelSpec> channels, const Config &config, u16 mtu, Sink &sink, const CoreHooks &hooks,
           std::span<u8> storage) noexcept
    : m_sink(sink), m_hooks(hooks), m_config(config), m_mtu(mtu), m_config_hash(meaproto::config_hash(channels)),
      m_peer_mtu(mtu)
{
    // 把注入的存储切成一通道一条环形队列；装不下或超出条目上限的通道不登记
    u8 *cursor = storage.data();
    usize remaining = storage.size();
    for (const ChannelSpec &spec : channels)
    {
        if (m_channel_count >= MAX_CHANNELS || spec.queue_depth == 0)
        {
            break;
        }

        const usize bytes = entry_stride(spec) * spec.queue_depth;
        if (remaining < bytes)
        {
            break;
        }

        Channel &channel = m_channels[m_channel_count++];
        channel.spec = spec;
        channel.queue = cursor;
        cursor += bytes;
        remaining -= bytes;
    }
}

usize Core::entry_stride(const ChannelSpec &spec) noexcept
{
    const usize raw = ENTRY_HEADER + spec.max_payload;
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

usize Core::storage_bytes(std::span<const ChannelSpec> channels) noexcept
{
    usize total = 0;
    for (const ChannelSpec &spec : channels)
    {
        total += entry_stride(spec) * spec.queue_depth;
    }
    return total;
}

Core::Channel *Core::find_channel(ChannelId channel) noexcept
{
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        if (m_channels[i].spec.id == channel)
        {
            return &m_channels[i];
        }
    }
    return nullptr;
}

const Core::Channel *Core::find_channel(ChannelId channel) const noexcept
{
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        if (m_channels[i].spec.id == channel)
        {
            return &m_channels[i];
        }
    }
    return nullptr;
}

SendResult Core::send(ChannelId channel, std::span<const u8> data) noexcept
{
    if (m_state != State::Connected)
    {
        return SendResult::NotConnected;
    }

    Channel *slot = find_channel(channel);
    if (slot == nullptr)
    {
        return SendResult::InvalidChannel;
    }

    if (data.size() > slot->spec.max_payload)
    {
        return SendResult::TooLarge;
    }

    if (slot->count >= slot->spec.queue_depth)
    {
        return SendResult::Busy;
    }

    u8 *entry = slot->queue + static_cast<usize>(slot->tail) * entry_stride(slot->spec);
    const u16 len = static_cast<u16>(data.size());
    std::memcpy(entry, &len, sizeof(len));
    std::memcpy(entry + ENTRY_HEADER, data.data(), len);

    slot->tail = static_cast<u16>((slot->tail + 1) % slot->spec.queue_depth);
    ++slot->count;
    return SendResult::Ok;
}

bool Core::can_send(ChannelId channel) const noexcept
{
    const Channel *slot = find_channel(channel);
    return slot != nullptr && slot->count < slot->spec.queue_depth;
}

void Core::open(u32 now) noexcept
{
    if (m_state != State::Idle || time_before(now, m_handshake_gate_ms))
    {
        return;
    }

    m_own_epoch = next_epoch();
    m_peer_epoch = 0;
    m_peer_mtu = m_mtu;
    m_state = State::Connecting;
    m_handshake_retries = 0;
    m_handshake_deadline_ms = now + retry_ms();
    (void)send_control(now, ControlType::ConnectRequest, ControlStatus::Ok, 0, 0);
}

void Core::close(u32 now) noexcept
{
    if (m_state == State::Idle)
    {
        return;
    }

    (void)send_control(now, ControlType::Disconnect, ControlStatus::Ok, m_peer_epoch, 0);
    m_handshake_gate_ms = 0;
    enter_idle(DisconnectReason::Requested);
}

void Core::receive(u32 now, std::span<const u8> frame) noexcept
{
    if (frame.size() < 2)
    {
        return;
    }

    if ((frame[0] & FLAG_MASK & FLAG_CONTROL) == 0)
    {
        return;
    }

    if (static_cast<u8>(frame[0] >> VERSION_SHIFT) != VERSION)
    {
        (void)reject_version(now, frame);
        return;
    }

    ControlFrame control;
    if (decode(frame, control) != WireError::Ok)
    {
        return;
    }

    handle_control(now, control);
}

u32 Core::step(u32 now) noexcept
{
    if (m_state == State::Connecting && !time_before(now, m_handshake_deadline_ms))
    {
        if (m_handshake_retries >= m_config.connect_max_retries)
        {
            m_handshake_gate_ms = now + retry_ms();
            enter_idle(DisconnectReason::Rejected);
        }
        else
        {
            ++m_handshake_retries;
            m_handshake_deadline_ms = now + retry_ms();
            (void)send_control(now, ControlType::ConnectRequest, ControlStatus::Ok, 0, 0);
        }
    }

    return next_wake(now);
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

u16 Core::mtu() const noexcept
{
    return m_peer_mtu;
}

u32 Core::own_epoch() const noexcept
{
    return m_own_epoch;
}

u32 Core::peer_epoch() const noexcept
{
    return m_peer_epoch;
}

u16 Core::config_hash() const noexcept
{
    return m_config_hash;
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

void Core::handle_control(u32 now, const ControlFrame &frame) noexcept
{
    switch (frame.type)
    {
    case ControlType::ConnectRequest:
        handle_connect_request(now, frame);
        break;
    case ControlType::ConnectAccept:
        handle_connect_accept(frame);
        break;
    case ControlType::ConnectReject:
        handle_connect_reject(now, frame);
        break;
    case ControlType::Disconnect:
        handle_disconnect(now, frame);
        break;
    default:
        break;
    }
}

void Core::handle_connect_request(u32 now, const ControlFrame &frame) noexcept
{
    if (frame.config_hash != m_config_hash)
    {
        (void)send_control(now, ControlType::ConnectReject, ControlStatus::ConfigMismatch, frame.own_epoch, 0);
        return;
    }

    const bool already_connected = (m_state == State::Connected);
    if (already_connected && frame.own_epoch == m_peer_epoch)
    {
        // 对端没收到 Accept：重发一份，不重置连接
        (void)send_control(now, ControlType::ConnectAccept, ControlStatus::Ok, m_peer_epoch, 0);
        return;
    }

    m_own_epoch = next_epoch();
    m_peer_epoch = frame.own_epoch;
    m_peer_mtu = frame.mtu < m_mtu ? frame.mtu : m_mtu;
    m_state = State::Connected;
    m_handshake_retries = 0;
    (void)send_control(now, ControlType::ConnectAccept, ControlStatus::Ok, m_peer_epoch, 0);
    if (!already_connected)
    {
        m_sink.on_connected();
    }
}

void Core::handle_connect_accept(const ControlFrame &frame) noexcept
{
    if (m_state != State::Connecting || frame.peer_epoch != m_own_epoch)
    {
        return;
    }

    m_peer_epoch = frame.own_epoch;
    m_peer_mtu = frame.mtu < m_mtu ? frame.mtu : m_mtu;
    m_state = State::Connected;
    m_handshake_retries = 0;
    m_sink.on_connected();
}

void Core::handle_connect_reject(u32 now, const ControlFrame &frame) noexcept
{
    if (m_state != State::Connecting || frame.peer_epoch != m_own_epoch)
    {
        return;
    }

    m_handshake_gate_ms = now + retry_ms();
    enter_idle(DisconnectReason::Rejected);
}

void Core::handle_disconnect(u32 now, const ControlFrame &frame) noexcept
{
    if (m_state == State::Idle)
    {
        return;
    }
    if (m_state == State::Connected && frame.own_epoch != m_peer_epoch)
    {
        // 旧会话的断开：不能拿它拆掉当前连接
        return;
    }

    m_handshake_gate_ms = now + retry_ms();
    enter_idle(DisconnectReason::Requested);
}

bool Core::reject_version(u32 now, std::span<const u8> frame) noexcept
{
    if (frame[1] != static_cast<u8>(ControlType::ConnectRequest))
    {
        return false;
    }
    return send_control(now, ControlType::ConnectReject, ControlStatus::VersionMismatch, 0, 0);
}

void Core::enter_idle(DisconnectReason reason) noexcept
{
    const bool was_active = (m_state != State::Idle);
    m_state = State::Idle;
    m_own_epoch = 0;
    m_peer_epoch = 0;
    m_peer_mtu = m_mtu;
    m_handshake_retries = 0;
    m_handshake_deadline_ms = 0;
    if (was_active)
    {
        m_sink.on_disconnected(reason);
    }
}

u32 Core::next_wake(u32 now) const noexcept
{
    u32 wake = now + IDLE_WAKE_MS;
    if (m_state == State::Connecting && time_before(m_handshake_deadline_ms, wake))
    {
        wake = m_handshake_deadline_ms;
    }
    return wake;
}

u32 Core::retry_ms() const noexcept
{
    return (m_config.connect_retry_ms == 0) ? 1 : m_config.connect_retry_ms;
}

u32 Core::next_epoch() noexcept
{
    const u32 epoch = m_hooks.random != nullptr ? m_hooks.random(m_hooks.ctx) : 0;
    return epoch == 0 ? 1 : epoch;
}
} // namespace meaproto
