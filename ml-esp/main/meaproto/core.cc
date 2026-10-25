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
        const u16 reorder = reorder_capacity(spec);
        const usize queue_bytes = entry_stride(spec, m_config, mtu) * spec.queue_depth;
        const usize window_bytes = frame_stride(mtu) * window;
        const usize reorder_bytes = reorder_stride(spec, m_config) * reorder;
        if (window > MAX_WINDOW || remaining < queue_bytes + window_bytes + reorder_bytes)
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
        channel.reorder = cursor;
        channel.reorder_count = reorder;
        cursor += reorder_bytes;
        remaining -= reorder_bytes;
    }

    // 重组区紧跟在通道区之后
    const usize used = m_reassembly.init(std::span<u8>{cursor, remaining}, config, mtu);
    cursor += used;
    remaining -= used;

    m_frame = cursor;
    m_frame_cap = static_cast<u16>(remaining < mtu ? remaining : mtu);
}

u16 Core::frame_payload_capacity(Reliability reliability, u16 mtu) noexcept
{
    const usize header = DATAGRAM_HEADER_SIZE + MESSAGE_HEADER_BASE_SIZE +
                         ((reliability == Reliability::ReliableOrdered) ? MESSAGE_ORDERING_SIZE : 0);
    return (mtu > header) ? static_cast<u16>(mtu - header) : 0;
}

usize Core::message_limit(const ChannelSpec &spec, const Config &config) noexcept
{
    return (spec.max_payload < config.max_message_size) ? spec.max_payload : config.max_message_size;
}

usize Core::entry_stride(const ChannelSpec &spec, const Config &config, u16 mtu) noexcept
{
    const usize limit = message_limit(spec, config);
    const usize capacity = frame_payload_capacity(spec.reliability, mtu);
    const usize raw = ENTRY_HEADER + ((limit < capacity) ? limit : capacity);
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

usize Core::frame_stride(u16 mtu) noexcept
{
    const usize raw = SLOT_HEADER + mtu;
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

usize Core::reorder_stride(const ChannelSpec &spec, const Config &config) noexcept
{
    const usize raw = 2 * sizeof(u16) + message_limit(spec, config);
    return (raw + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}

u16 Core::reorder_capacity(const ChannelSpec &spec) noexcept
{
    const u16 wanted = (spec.reorder_depth != 0) ? spec.reorder_depth : spec.queue_depth;
    const u16 limit = static_cast<u16>(ACK_BITS_WIDTH);
    return (wanted < limit) ? wanted : limit;
}

usize Core::storage_bytes(std::span<const ChannelSpec> channels, const Config &config, u16 mtu) noexcept
{
    usize total = frame_stride(mtu); // 组帧暂存
    for (const ChannelSpec &spec : channels)
    {
        const u16 window = (spec.window != 0) ? spec.window : config.datagram_window;
        const u16 reorder = reorder_capacity(spec);
        total += entry_stride(spec, config, mtu) * spec.queue_depth;
        total += frame_stride(mtu) * (window > MAX_WINDOW ? MAX_WINDOW : window);
        total += reorder_stride(spec, config) * reorder;
    }
    return total + Reassembler::bytes(config, mtu);
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

    if (data.size() > message_limit(slot->spec, m_config))
    {
        return SendResult::TooLarge;
    }

    const bool reliable = (slot->spec.reliability == Reliability::ReliableOrdered);
    const u16 chunk =
        reliable ? fragment_payload_limit(m_peer_mtu) : frame_payload_capacity(slot->spec.reliability, m_peer_mtu);
    if (chunk == 0 || (!reliable && data.size() > chunk))
    {
        return SendResult::TooLarge;
    }

    const usize chunks = (data.size() == 0) ? 1 : (data.size() + chunk - 1) / chunk;
    if (static_cast<usize>(slot->count) + chunks > slot->spec.queue_depth)
    {
        return SendResult::Busy;
    }

    const u16 ordering = slot->next_ordering;
    if (reliable)
    {
        ++slot->next_ordering;
    }

    const u16 split_id = (reliable && chunks > 1) ? slot->next_split_id : 0;
    if (split_id != 0)
    {
        ++slot->next_split_id;
        if (slot->next_split_id == 0)
        {
            slot->next_split_id = 1;
        }
    }

    const usize stride = entry_stride(slot->spec, m_config, m_mtu);
    for (usize index = 0; index < chunks; ++index)
    {
        const usize offset = index * chunk;
        const usize len = (data.size() - offset < chunk) ? (data.size() - offset) : chunk;

        ChunkHeader header;
        header.len = static_cast<u16>(len);
        header.ordering = ordering;
        header.split_id = split_id;
        header.split_index = static_cast<u16>(index);
        header.split_count = static_cast<u16>(chunks);

        u8 *entry = slot->queue + static_cast<usize>(slot->tail) * stride;
        std::memcpy(entry, &header, sizeof(header));
        if (len != 0)
        {
            std::memcpy(entry + ENTRY_HEADER, data.data() + offset, len);
        }

        slot->tail = static_cast<u16>((slot->tail + 1) % slot->spec.queue_depth);
        ++slot->count;
    }
    return SendResult::Ok;
}

bool Core::can_send(ChannelId channel) const noexcept
{
    const Channel *slot = find_channel(channel);
    return slot != nullptr && slot->count < slot->spec.queue_depth;
}

bool Core::set_reliability(ChannelId channel, Reliability reliability) noexcept
{
    if (m_state != State::Connected)
    {
        return false;
    }

    Channel *slot = find_channel(channel);
    if (slot == nullptr)
    {
        return false;
    }

    slot->spec.reliability = reliability;
    slot->reset();
    m_reassembly.clear(channel); // 该通道未收齐的分片随序号一起作废
    return true;
}

void Core::Channel::reset() noexcept
{
    head = 0;
    tail = 0;
    count = 0;
    used_slots = 0;
    tx_seq = 0;
    next_ordering = 0;
    next_split_id = 1;
    for (WindowSlot &slot : slots)
    {
        slot = WindowSlot{};
    }
    reorder_used = 0;
    rx_expected = 0;
    rx_bits = 0;
    rx_ordering = 0;
    ack_pending = false;
    ack_deadline_ms = 0;
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

    const u8 *entry = channel.queue + static_cast<usize>(channel.head) * entry_stride(channel.spec, m_config, m_mtu);
    ChunkHeader chunk;
    std::memcpy(&chunk, entry, sizeof(chunk));

    DatagramHeader header;
    header.flags = FLAG_HAS_DATA;
    header.channel = channel.spec.id.value;
    header.message_count = 1;
    header.ack_seq = channel.rx_expected;
    header.ack_bits = channel.rx_bits;
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
    message.payload_len = chunk.len;
    message.reliability = channel.spec.reliability;
    if (reliable)
    {
        message.ordering = chunk.ordering;
        if (chunk.split_count >= MIN_SPLIT_COUNT)
        {
            message.fragmented = true;
            message.split_id = chunk.split_id;
            message.split_index = chunk.split_index;
            message.split_count = chunk.split_count;
        }
    }

    usize offset = written;
    if (encode(message, std::span<u8>{m_frame + offset, m_frame_cap - offset}, written) != WireError::Ok)
    {
        return false;
    }
    offset += written;

    if (offset + chunk.len > m_frame_cap)
    {
        return false;
    }
    std::memcpy(m_frame + offset, entry + ENTRY_HEADER, chunk.len);
    const usize frame_len = offset + chunk.len;

    if (!m_hooks.send(m_hooks.ctx, std::span<const u8>{m_frame, frame_len}))
    {
        return false;
    }
    channel.ack_pending = false;

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
        std::memcpy(bytes + SLOT_HEADER, m_frame, frame_len);

        slot->used = true;
        slot->seq = header.datagram_seq;
        slot->len = stored;
        slot->retx = 0;
        slot->sent_ms = now;
        slot->deadline_ms = now + rto_backoff(0);
        ++channel.used_slots;
        channel.tx_seq = static_cast<u16>(channel.tx_seq + 1);
    }

    channel.head = static_cast<u16>((channel.head + 1) % channel.spec.queue_depth);
    --channel.count;
    ++m_stats.datagrams_sent;
    m_last_tx_ms = now;
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
            if (!m_hooks.send(m_hooks.ctx, std::span<const u8>{bytes + SLOT_HEADER, slot.len}))
            {
                return any;
            }

            slot.retx = (slot.retx < 0xFF) ? static_cast<u8>(slot.retx + 1) : 0xFF;
            slot.deadline_ms = now + rto_backoff(slot.retx);
            ++m_stats.datagrams_retransmitted;
            m_last_tx_ms = now;
            any = true;
        }
    }
    return any;
}

void Core::advance_datagrams(u32 now) noexcept
{
    flush_acks(now);
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

void Core::handle_datagram(u32 now, std::span<const u8> frame) noexcept
{
    DatagramHeader header;
    if (decode(frame, header) != WireError::Ok || header.channel == 0 || frame.size() < DATAGRAM_HEADER_SIZE)
    {
        return;
    }

    Channel *channel = find_channel(ChannelId{header.channel});
    if (channel == nullptr)
    {
        return;
    }

    handle_ack(*channel, header.ack_seq, header.ack_bits, now);
    m_last_rx_ms = now;

    const bool needs_ack = (header.flags & FLAG_NEEDS_ACK) != 0;
    u16 expected = channel->rx_expected;
    u32 bits = channel->rx_bits;

    if (needs_ack)
    {
        const u16 back = static_cast<u16>(expected - header.datagram_seq);
        const u16 ahead = static_cast<u16>(header.datagram_seq - expected);

        bool duplicate = (back != 0 && back < 0x8000);
        if (!duplicate && ahead >= 1 && ahead <= 32 && (bits & (1u << (ahead - 1))) != 0)
        {
            duplicate = true;
        }

        if (duplicate)
        {
            // 重复帧：补发一次确认，让对端释放窗口
            ++m_stats.duplicate_datagrams;
            channel->ack_pending = true;
            channel->ack_deadline_ms = now + m_config.ack_delay_ms;
            return;
        }

        if (ahead > 32)
        {
            ++m_stats.out_of_window;
            return;
        }

        if (ahead == 0)
        {
            // 位图 bit i 恒表示 expected + 1 + i；低位连续的 1 是紧随其后的已收帧，一并跳过
            u32 ones = 0;
            while (ones < 32 && ((bits >> ones) & 1u) != 0)
            {
                ++ones;
            }
            const u32 shift = ones + 1;
            bits = (shift >= 32) ? 0u : (bits >> shift);
            expected = static_cast<u16>(expected + shift);
        }
        else
        {
            bits |= (1u << (ahead - 1));
        }
    }

    // 逐条消息交付；重排缓冲满时本帧不确认，靠对端重传补齐
    BinDeserializer in{frame.data() + DATAGRAM_HEADER_SIZE, frame.size() - DATAGRAM_HEADER_SIZE};
    bool overflow = false;
    for (u8 i = 0; i < header.message_count && !overflow; ++i)
    {
        MessageHeader message;
        // 消息头布局由通道的可靠性决定（线上不单独携带）
        message.reliability = channel->spec.reliability;
        if (message.deserialize(in) != WireError::Ok || in.remaining() < message.payload_len)
        {
            return;
        }

        if (message.fragmented)
        {
            // 槽满或交付被打回时本帧不确认，等对端重传
            if (!m_reassembly.push(channel->spec.id, message, in.current(), fragment_payload_limit(m_peer_mtu),
                                   &Core::deliver_hook, this))
            {
                return;
            }
            in.consume(message.payload_len);
            continue;
        }

        deliver_message(*channel, message.ordering, in.current(), message.payload_len, overflow);
        in.consume(message.payload_len);
    }

    if (overflow)
    {
        ++m_stats.messages_dropped;
        return;
    }

    if (needs_ack)
    {
        channel->rx_expected = expected;
        channel->rx_bits = bits;
        channel->ack_pending = true;
        channel->ack_deadline_ms = now + m_config.ack_delay_ms;
    }
}

bool Core::deliver_hook(void *ctx, ChannelId channel, u16 ordering, const u8 *data, usize len) noexcept
{
    auto &self = *static_cast<Core *>(ctx);
    Channel *slot = self.find_channel(channel);
    if (slot == nullptr)
    {
        return true; // 通道已经不在：当作已消费
    }

    bool overflow = false;
    self.deliver_message(*slot, ordering, data, static_cast<u16>(len), overflow);
    return !overflow;
}

void Core::deliver_message(Channel &channel, u16 ordering, const u8 *data, u16 len, bool &overflow) noexcept
{
    if (channel.spec.reliability != Reliability::ReliableOrdered)
    {
        m_sink.on_message(channel.spec.id, std::span<const u8>{data, len});
        ++m_stats.messages_delivered;
        return;
    }

    const u16 ahead = static_cast<u16>(ordering - channel.rx_ordering);
    if (ahead == 0)
    {
        m_sink.on_message(channel.spec.id, std::span<const u8>{data, len});
        ++m_stats.messages_delivered;
        channel.rx_ordering = static_cast<u16>(channel.rx_ordering + 1);
        drain_reorder(channel);
        return;
    }

    if (ahead >= 0x8000)
    {
        return; // 旧消息：已经交付过
    }

    if (channel.reorder_used >= channel.reorder_count)
    {
        overflow = true;
        ++m_stats.reorder_overflow;
        return;
    }

    u8 *slot = channel.reorder + static_cast<usize>(channel.reorder_used) * reorder_stride(channel.spec, m_config);
    std::memcpy(slot, &ordering, sizeof(ordering));
    std::memcpy(slot + sizeof(u16), &len, sizeof(len));
    std::memcpy(slot + 2 * sizeof(u16), data, len);
    ++channel.reorder_used;
}

void Core::drain_reorder(Channel &channel) noexcept
{
    const usize stride = reorder_stride(channel.spec, m_config);
    bool moved = true;
    while (moved)
    {
        moved = false;
        for (u16 i = 0; i < channel.reorder_used; ++i)
        {
            u8 *slot = channel.reorder + static_cast<usize>(i) * stride;
            u16 ordering = 0;
            std::memcpy(&ordering, slot, sizeof(ordering));
            if (ordering != channel.rx_ordering)
            {
                continue;
            }

            u16 len = 0;
            std::memcpy(&len, slot + sizeof(u16), sizeof(len));
            m_sink.on_message(channel.spec.id, std::span<const u8>{slot + 2 * sizeof(u16), len});
            ++m_stats.messages_delivered;
            channel.rx_ordering = static_cast<u16>(channel.rx_ordering + 1);

            --channel.reorder_used;
            if (i != channel.reorder_used)
            {
                u8 *last = channel.reorder + static_cast<usize>(channel.reorder_used) * stride;
                std::memcpy(slot, last, stride);
            }
            moved = true;
            break;
        }
    }
}

bool Core::send_ack(Channel &channel, u32 now) noexcept
{
    if (m_frame == nullptr)
    {
        return false;
    }

    DatagramHeader header;
    header.flags = FLAG_ACK_ONLY;
    header.channel = channel.spec.id.value;
    header.message_count = 0;
    header.ack_seq = channel.rx_expected;
    header.ack_bits = channel.rx_bits;

    usize written = 0;
    if (encode(header, std::span<u8>{m_frame, m_frame_cap}, written) != WireError::Ok ||
        !m_hooks.send(m_hooks.ctx, std::span<const u8>{m_frame, written}))
    {
        return false;
    }

    channel.ack_pending = false;
    m_last_tx_ms = now;
    return true;
}

void Core::flush_acks(u32 now) noexcept
{
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        Channel &channel = m_channels[i];
        if (!channel.ack_pending || time_before(now, channel.ack_deadline_ms))
        {
            continue;
        }

        if (!send_ack(channel, now))
        {
            return;
        }
    }
}

void Core::handle_ack(Channel &channel, u16 ack_seq, u32 ack_bits, u32 now) noexcept
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

        if (slot.retx == 0)
        {
            // 未重传过的帧才采 RTT（Karn）
            sample_rtt(static_cast<u32>(now - slot.sent_ms));
        }

        slot.used = false;
        --channel.used_slots;
        ++m_stats.datagrams_acked;
    }
}

void Core::sample_rtt(u32 sample) noexcept
{
    if (sample == 0 || sample > m_config.connect_timeout_ms)
    {
        return;
    }

    m_rtt_ms = (m_rtt_ms == 0) ? sample : (m_rtt_ms * 7 + sample) / 8;

    u32 rto = m_rtt_ms * 2;
    if (rto < m_config.rto_initial_ms)
    {
        rto = m_config.rto_initial_ms;
    }
    m_rto_ms = (rto > m_config.rto_max_ms) ? m_config.rto_max_ms : rto;
}

bool Core::ping(u32 now) noexcept
{
    if (m_state != State::Connected)
    {
        return false;
    }

    m_ping_in_flight = m_ping_token;
    m_ping_sent_ms = now;
    ++m_ping_token;
    if (m_ping_token == 0)
    {
        m_ping_token = 1;
    }
    return send_control(now, ControlType::Ping, ControlStatus::Ok, m_peer_epoch, m_ping_in_flight);
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
            handle_datagram(now, frame);
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
        if (static_cast<u32>(now - m_last_rx_ms) > m_config.connect_timeout_ms)
        {
            enter_idle(DisconnectReason::Timeout);
        }
        else
        {
            if (static_cast<u32>(now - m_last_tx_ms) >= m_config.keepalive_interval_ms)
            {
                (void)send_control(now, ControlType::KeepAlive, ControlStatus::Ok, m_peer_epoch, 0);
            }
            advance_datagrams(now);
        }
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
    stats.fragments_reassembled = m_reassembly.completed();
    stats.messages_dropped += m_reassembly.rejected();
    stats.connected = connected();
    stats.peer_epoch = static_cast<u16>(m_peer_epoch);
    stats.datagrams_in_flight = in_flight;
    stats.rtt_ms = m_rtt_ms;
    stats.rto_ms = (m_rto_ms == 0) ? m_config.rto_initial_ms : m_rto_ms;
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
    m_last_rx_ms = now;

    switch (frame.type)
    {
    case ControlType::ConnectRequest:
        handle_connect_request(now, frame);
        break;
    case ControlType::ConnectAccept:
        handle_connect_accept(now, frame);
        break;
    case ControlType::ConnectReject:
        handle_connect_reject(now, frame);
        break;
    case ControlType::Disconnect:
        handle_disconnect(now, frame);
        break;
    case ControlType::KeepAlive:
        (void)send_control(now, ControlType::KeepAliveAck, ControlStatus::Ok, m_peer_epoch, 0);
        break;
    case ControlType::Ping:
        (void)send_control(now, ControlType::Pong, ControlStatus::Ok, m_peer_epoch, frame.token);
        break;
    case ControlType::Pong:
        if (m_ping_in_flight != 0 && frame.token == m_ping_in_flight)
        {
            sample_rtt(static_cast<u32>(now - m_ping_sent_ms));
            m_ping_in_flight = 0;
        }
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
    m_last_rx_ms = now;
    m_last_tx_ms = now;
    (void)send_control(now, ControlType::ConnectAccept, ControlStatus::Ok, m_peer_epoch, 0);
    if (!already_connected)
    {
        m_sink.on_connected();
    }
}

void Core::handle_connect_accept(u32 now, const ControlFrame &frame) noexcept
{
    if (m_state != State::Connecting || frame.peer_epoch != m_own_epoch)
    {
        return;
    }

    m_peer_epoch = frame.own_epoch;
    m_peer_mtu = frame.mtu < m_mtu ? frame.mtu : m_mtu;
    m_state = State::Connected;
    m_handshake_retries = 0;
    m_last_rx_ms = now;
    m_last_tx_ms = now;
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
    for (u8 i = 0; i < m_channel_count; ++i)
    {
        m_channels[i].reset();
    }
    m_reassembly.clear();
    m_ping_in_flight = 0;
    m_ping_sent_ms = 0;
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
    if (m_state == State::Connected)
    {
        const u32 rx_deadline = m_last_rx_ms + m_config.connect_timeout_ms + 1;
        if (time_before(rx_deadline, wake))
        {
            wake = rx_deadline;
        }
        const u32 keepalive = m_last_tx_ms + m_config.keepalive_interval_ms;
        if (time_before(keepalive, wake))
        {
            wake = keepalive;
        }
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
