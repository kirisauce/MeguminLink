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
    // 把注入的存储切成每通道一条环形队列加一段窗口帧槽；装不下的通道不登记
    u8 *cursor = storage.data();
    usize remaining = storage.size();
    for (const ChannelSpec &spec : channels)
    {
        if (m_channel_count >= MAX_CHANNELS || spec.queue_depth == 0)
        {
            break;
        }

        const u16 window = (spec.window != 0) ? spec.window : m_config.datagram_window;
        const usize queue_bytes = entry_stride(spec) * spec.queue_depth;
        const usize window_bytes = frame_stride(mtu) * window;
        if (window > MAX_WINDOW || remaining < queue_bytes + window_bytes)
        {
            break;
        }

        Channel &channel = m_channels[m_channel_count++];
        channel.spec = spec;
        channel.queue = cursor;
        cursor += queue_bytes;
        remaining -= queue_bytes;
        channel.window = cursor;
        channel.window_count = window;
        cursor += window_bytes;
        remaining -= window_bytes;
    }

    m_frame = cursor;
    m_frame_cap = static_cast<u16>(remaining < mtu ? remaining : mtu);
}

usize Core::entry_stride(const ChannelSpec &spec) noexcept
{
    const usize raw = ENTRY_HEADER + spec.max_payload;
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

usize Core::frame_stride(u16 mtu) noexcept
{
    const usize raw = ENTRY_HEADER + mtu;
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

usize Core::storage_bytes(std::span<const ChannelSpec> channels, const Config &config, u16 mtu) noexcept
{
    usize total = frame_stride(mtu); // 组帧暂存
    for (const ChannelSpec &spec : channels)
    {
        const u16 window = (spec.window != 0) ? spec.window : config.datagram_window;
        total += entry_stride(spec) * spec.queue_depth;
        total += frame_stride(mtu) * (window > MAX_WINDOW ? MAX_WINDOW : window);
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

u32 Core::rto_backoff(u8 retx) const noexcept
{
    u32 ms = m_config.rto_initial_ms;
    for (u8 i = 0; i < retx && ms < m_config.rto_max_ms; ++i)
    {
        ms *= 2;
    }
    return ms < m_config.rto_max_ms ? ms : m_config.rto_max_ms;
}

bool Core::transmit(Channel &channel, u32 now) noexcept
{
    if (channel.count == 0 || m_frame == nullptr)
    {
        return false;
    }

    const bool reliable = (channel.spec.reliability == Reliability::ReliableOrdered);
    if (reliable && channel.used_slots >= channel.window_count)
    {
        return false;
    }

    const u8 *entry = channel.queue + static_cast<usize>(channel.head) * entry_stride(channel.spec);
    u16 payload_len = 0;
    std::memcpy(&payload_len, entry, sizeof(payload_len));

    DatagramHeader header;
    header.flags = FLAG_HAS_DATA;
    header.channel = channel.spec.id.value;
    header.message_count = 1;
    if (reliable)
    {
        header.flags |= FLAG_NEEDS_ACK;
        header.datagram_seq = channel.tx_seq;
    }

    usize written = 0;
    if (encode(header, std::span<u8>{m_frame, m_frame_cap}, written) != WireError::Ok)
    {
        return false;
    }

    MessageHeader message;
    message.payload_len = payload_len;
    message.reliability = channel.spec.reliability;
    if (reliable)
    {
        message.ordering = channel.tx_ordering;
    }

    usize offset = written;
    if (encode(message, std::span<u8>{m_frame + offset, m_frame_cap - offset}, written) != WireError::Ok)
    {
        return false;
    }
    offset += written;

    if (offset + payload_len > m_frame_cap)
    {
        return false;
    }
    std::memcpy(m_frame + offset, entry + ENTRY_HEADER, payload_len);
    const usize frame_len = offset + payload_len;

    if (!m_hooks.send(m_hooks.ctx, std::span<const u8>{m_frame, frame_len}))
    {
        return false;
    }

    if (reliable)
    {
        const usize stride = frame_stride(m_mtu);
        WindowSlot *slot = nullptr;
        u16 index = 0;
        for (u16 i = 0; i < channel.window_count; ++i)
        {
            if (!channel.slots[i].used)
            {
                slot = &channel.slots[i];
                index = i;
                break;
            }
        }
        if (slot == nullptr)
        {
            return false;
        }

        u8 *bytes = channel.window + static_cast<usize>(index) * stride;
        const u16 stored = static_cast<u16>(frame_len);
        std::memcpy(bytes, &stored, sizeof(stored));
        std::memcpy(bytes + ENTRY_HEADER, m_frame, frame_len);

        slot->used = true;
        slot->seq = header.datagram_seq;
        slot->len = stored;
        slot->retx = 0;
        slot->deadline_ms = now + rto_backoff(0);
        ++channel.used_slots;
        channel.tx_seq = static_cast<u16>(channel.tx_seq + 1);
        channel.tx_ordering = static_cast<u16>(channel.tx_ordering + 1);
    }

    channel.head = static_cast<u16>((channel.head + 1) % channel.spec.queue_depth);
    --channel.count;
    ++m_stats.datagrams_sent;
    return true;
}

bool Core::retransmit(u32 now) noexcept
{
    bool any = false;
    const usize stride = frame_stride(m_mtu);
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        Channel &channel = m_channels[i];
        for (u16 s = 0; s < channel.window_count; ++s)
        {
            WindowSlot &slot = channel.slots[s];
            if (!slot.used || time_before(now, slot.deadline_ms))
            {
                continue;
            }

            const u8 *bytes = channel.window + static_cast<usize>(s) * stride;
            if (!m_hooks.send(m_hooks.ctx, std::span<const u8>{bytes + ENTRY_HEADER, slot.len}))
            {
                return any;
            }

            slot.retx = (slot.retx < 0xFF) ? static_cast<u8>(slot.retx + 1) : 0xFF;
            slot.deadline_ms = now + rto_backoff(slot.retx);
            ++m_stats.datagrams_retransmitted;
            any = true;
        }
    }
    return any;
}

void Core::advance_datagrams(u32 now) noexcept
{
    (void)retransmit(now);

    for (u8 sent = 0; sent < m_config.send_quota_per_tick; ++sent)
    {
        Channel *best = nullptr;
        for (u8 i = 0; i < m_channel_count; ++i)
        {
            Channel &channel = m_channels[i];
            if (channel.count == 0)
            {
                continue;
            }
            if (channel.spec.reliability == Reliability::ReliableOrdered && channel.used_slots >= channel.window_count)
            {
                continue;
            }
            if (best == nullptr || channel.spec.priority < best->spec.priority)
            {
                best = &channel;
            }
        }

        if (best == nullptr || !transmit(*best, now))
        {
            break;
        }
    }
}

void Core::handle_datagram(std::span<const u8> frame) noexcept
{
    DatagramHeader header;
    if (decode(frame, header) != WireError::Ok || header.channel == 0)
    {
        return;
    }

    Channel *channel = find_channel(ChannelId{header.channel});
    if (channel == nullptr)
    {
        return;
    }

    handle_ack(*channel, header.ack_seq, header.ack_bits);
}

void Core::handle_ack(Channel &channel, u16 ack_seq, u32 ack_bits) noexcept
{
    for (u16 s = 0; s < channel.window_count; ++s)
    {
        WindowSlot &slot = channel.slots[s];
        if (!slot.used)
        {
            continue;
        }

        const u16 back = static_cast<u16>(ack_seq - slot.seq);
        bool acked = (back != 0 && back < 0x8000);
        if (!acked)
        {
            const u16 ahead = static_cast<u16>(slot.seq - ack_seq);
            acked = (ahead >= 1 && ahead <= 32 && (ack_bits & (1u << (ahead - 1))) != 0);
        }
        if (!acked)
        {
            continue;
        }

        slot.used = false;
        --channel.used_slots;
        ++m_stats.datagrams_acked;
    }
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
        if (m_state == State::Connected)
        {
            handle_datagram(frame);
        }
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
    if (m_state == State::Connected)
    {
        advance_datagrams(now);
    }

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
    u32 in_flight = 0;
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        in_flight += m_channels[i].used_slots;
    }

    Stats stats = m_stats;
    stats.connected = connected();
    stats.peer_epoch = static_cast<u16>(m_peer_epoch);
    stats.datagrams_in_flight = in_flight;
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
