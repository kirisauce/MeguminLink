#include "meaproto/reassembly.hpp"

#include <cstring>

namespace meaproto
{
namespace
{
usize align_u16(usize value) noexcept
{
    return (value + sizeof(u16) - 1) & ~(sizeof(u16) - 1);
}
} // namespace

u16 Reassembler::capacity(const Config &config) noexcept
{
    return (config.max_reassembly_slots < MAX_REASSEMBLY_SLOTS) ? config.max_reassembly_slots
                                                                : static_cast<u16>(MAX_REASSEMBLY_SLOTS);
}

u16 Reassembler::max_fragments(const Config &config, u16 mtu) noexcept
{
    const u16 chunk = fragment_payload_limit(mtu);
    if (chunk == 0 || config.max_message_size == 0)
    {
        return 0;
    }
    return static_cast<u16>((config.max_message_size + chunk - 1) / chunk);
}

usize Reassembler::bytes(const Config &config, u16 mtu) noexcept
{
    const usize bits = (static_cast<usize>(max_fragments(config, mtu)) + 7) / 8;
    return align_u16(config.max_message_size + bits) * capacity(config);
}

usize Reassembler::init(std::span<u8> storage, const Config &config, u16 mtu) noexcept
{
    m_count = capacity(config);
    m_used = 0;
    m_data_bytes = config.max_message_size;
    m_bits_bytes = (static_cast<usize>(max_fragments(config, mtu)) + 7) / 8;
    m_stride = align_u16(m_data_bytes + m_bits_bytes);

    const usize total = m_stride * m_count;
    if (m_stride == 0 || storage.size() < total)
    {
        m_count = 0;
        return 0;
    }

    std::memset(storage.data(), 0, total);
    for (u8 i = 0; i < m_count; ++i)
    {
        Slot &slot = m_slots[i];
        slot.used = false;
        slot.data = storage.data() + static_cast<usize>(i) * m_stride;
        slot.received = slot.data + m_data_bytes;
    }
    return total;
}

void Reassembler::clear() noexcept
{
    for (u8 i = 0; i < m_count; ++i)
    {
        m_slots[i].used = false;
    }
    m_used = 0;
}

void Reassembler::clear(ChannelId channel) noexcept
{
    for (u8 i = 0; i < m_count; ++i)
    {
        Slot &slot = m_slots[i];
        if (slot.used && slot.channel == channel.value)
        {
            release(slot);
        }
    }
}

u32 Reassembler::completed() const noexcept
{
    return m_completed;
}

u32 Reassembler::rejected() const noexcept
{
    return m_rejected;
}

Reassembler::Slot *Reassembler::find(ChannelId channel, u16 split_id) noexcept
{
    for (u8 i = 0; i < m_count; ++i)
    {
        Slot &slot = m_slots[i];
        if (slot.used && slot.channel == channel.value && slot.split_id == split_id)
        {
            return &slot;
        }
    }
    return nullptr;
}

Reassembler::Slot *Reassembler::take() noexcept
{
    if (m_used >= m_count)
    {
        return nullptr;
    }

    for (u8 i = 0; i < m_count; ++i)
    {
        Slot &slot = m_slots[i];
        if (!slot.used)
        {
            slot.used = true;
            ++m_used;
            return &slot;
        }
    }
    return nullptr;
}

void Reassembler::release(Slot &slot) noexcept
{
    if (!slot.used)
    {
        return;
    }
    slot.used = false;
    --m_used;
}

bool Reassembler::flush_slot(Slot &slot, Deliver deliver, void *ctx) noexcept
{
    if (deliver != nullptr && deliver(ctx, ChannelId{slot.channel}, slot.ordering, slot.data, slot.total_len))
    {
        ++m_completed;
        release(slot);
        return true;
    }

    // 交付被打回：保留槽，等末片重传再试
    return false;
}

bool Reassembler::push(ChannelId channel, const MessageHeader &header, const u8 *payload, u16 chunk, Deliver deliver,
                       void *ctx) noexcept
{
    const bool invalid = (chunk == 0) || (header.payload_len == 0) || (header.payload_len > chunk) ||
                         ((header.split_index + 1 < header.split_count) && (header.payload_len != chunk)) ||
                         ((static_cast<usize>(header.split_index) * chunk + header.payload_len) > m_data_bytes);
    if (invalid)
    {
        ++m_rejected;
        return true;
    }

    Slot *slot = find(channel, header.split_id);
    if (slot == nullptr)
    {
        slot = take();
        if (slot == nullptr)
        {
            ++m_rejected;
            return false;
        }

        slot->channel = channel.value;
        slot->split_id = header.split_id;
        slot->split_count = header.split_count;
        slot->received_count = 0;
        slot->total_len = 0;
        slot->ordering = header.ordering;
        std::memset(slot->received, 0, m_bits_bytes);
    }
    else if (slot->split_count != header.split_count || slot->ordering != header.ordering)
    {
        ++m_rejected;
        return true;
    }

    const u16 index = header.split_index;
    u8 &bits = slot->received[index / 8];
    const u8 mask = static_cast<u8>(1u << (index % 8));
    if ((bits & mask) != 0)
    {
        return (slot->received_count == slot->split_count) ? flush_slot(*slot, deliver, ctx) : true;
    }

    bits |= mask;
    std::memcpy(slot->data + static_cast<usize>(index) * chunk, payload, header.payload_len);
    ++slot->received_count;
    const usize end = static_cast<usize>(index) * chunk + header.payload_len;
    if (end > slot->total_len)
    {
        slot->total_len = static_cast<u16>(end);
    }
    return (slot->received_count < slot->split_count) ? true : flush_slot(*slot, deliver, ctx);
}
} // namespace meaproto
