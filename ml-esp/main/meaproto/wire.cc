#include "meaproto/wire.hpp"

namespace meaproto
{
namespace
{
/** datagram 头的 flags 必须自洽：要么带数据、要么纯确认，两者互斥 */
bool datagram_flags_consistent(u8 flags, u8 message_count) noexcept
{
    const bool has_data = (flags & FLAG_HAS_DATA) != 0;
    const bool ack_only = (flags & FLAG_ACK_ONLY) != 0;
    if (has_data == ack_only)
    {
        return false;
    }
    return ack_only ? message_count == 0 : message_count >= 1;
}

bool control_type_valid(u8 value) noexcept
{
    return value <= static_cast<u8>(ControlType::Pong);
}

bool control_status_valid(u8 value) noexcept
{
    return value <= static_cast<u8>(ControlStatus::Timeout);
}

usize message_header_size(Reliability reliability, bool fragmented) noexcept
{
    usize size = MESSAGE_HEADER_BASE_SIZE;
    if (reliability != Reliability::ReliableOrdered)
    {
        return size;
    }
    size += MESSAGE_ORDERING_SIZE;
    if (fragmented)
    {
        size += MESSAGE_SPLIT_SIZE;
    }
    return size;
}
} // namespace

void DatagramHeader::serialize(BinSerializer &out) const
{
    out.write_u8(static_cast<u8>(((version & VERSION_MASK) << VERSION_SHIFT) | (flags & FLAG_MASK)));
    out.write_u8(static_cast<u8>(channel & CHANNEL_MASK));
    out.write_u16(datagram_seq);
    out.write_u8(message_count);
    out.write_u16(ack_seq);
    out.write_u32(ack_bits);
}

WireError DatagramHeader::deserialize(BinDeserializer &in)
{
    if (in.remaining() < DATAGRAM_HEADER_SIZE)
    {
        return WireError::Truncated;
    }

    const u8 framing = in.read_u8();
    const u8 framing_version = static_cast<u8>(framing >> VERSION_SHIFT);
    const u8 framing_flags = static_cast<u8>(framing & FLAG_MASK);
    if (framing_version != VERSION)
    {
        return WireError::BadVersion;
    }
    if ((framing_flags & FLAG_CONTROL) != 0)
    {
        return WireError::BadFlags;
    }

    const u8 framing_channel = static_cast<u8>(in.read_u8() & CHANNEL_MASK);
    const u16 seq = in.read_u16();
    const u8 count = in.read_u8();
    const u16 ack = in.read_u16();
    const u32 bits = in.read_u32();
    if (!datagram_flags_consistent(framing_flags, count))
    {
        return WireError::BadFlags;
    }

    version = framing_version;
    flags = framing_flags;
    channel = framing_channel;
    datagram_seq = seq;
    message_count = count;
    ack_seq = ack;
    ack_bits = bits;
    return WireError::Ok;
}

usize DatagramHeader::serialized_size() const noexcept
{
    return DATAGRAM_HEADER_SIZE;
}

void MessageHeader::serialize(BinSerializer &out) const
{
    out.write_u8(static_cast<u8>(fragmented ? MESSAGE_FLAG_FRAGMENTED : 0));
    out.write_u16(payload_len);
    if (reliability != Reliability::ReliableOrdered)
    {
        return;
    }
    out.write_u16(ordering);
    if (fragmented)
    {
        out.write_u16(split_id);
        out.write_u16(split_index);
        out.write_u16(split_count);
    }
}

WireError MessageHeader::deserialize(BinDeserializer &in)
{
    if (in.remaining() < MESSAGE_HEADER_BASE_SIZE)
    {
        return WireError::Truncated;
    }

    const u8 message_flags = in.read_u8();
    if ((message_flags & static_cast<u8>(~MESSAGE_FLAG_FRAGMENTED)) != 0)
    {
        return WireError::BadFlags;
    }

    const bool is_fragmented = (message_flags & MESSAGE_FLAG_FRAGMENTED) != 0;
    if (is_fragmented && reliability != Reliability::ReliableOrdered)
    {
        return WireError::BadFlags;
    }
    if (in.remaining() < message_header_size(reliability, is_fragmented) - 1)
    {
        return WireError::Truncated;
    }

    const u16 payload = in.read_u16();
    u16 ord = 0;
    u16 id = 0;
    u16 index = 0;
    u16 count = 0;
    if (reliability == Reliability::ReliableOrdered)
    {
        ord = in.read_u16();
        if (is_fragmented)
        {
            id = in.read_u16();
            index = in.read_u16();
            count = in.read_u16();
        }
    }
    if (is_fragmented)
    {
        if (count < MIN_SPLIT_COUNT)
        {
            return WireError::BadCount;
        }
        if (index >= count)
        {
            return WireError::BadLength;
        }
    }

    fragmented = is_fragmented;
    payload_len = payload;
    ordering = ord;
    split_id = id;
    split_index = index;
    split_count = count;
    return WireError::Ok;
}

usize MessageHeader::serialized_size() const noexcept
{
    return message_header_size(reliability, fragmented);
}

void ControlFrame::serialize(BinSerializer &out) const
{
    out.write_u8(static_cast<u8>((VERSION << VERSION_SHIFT) | FLAG_CONTROL));
    out.write_u8(static_cast<u8>(type));
    out.write_u8(static_cast<u8>(status));
    out.write_u32(peer_epoch);
    out.write_u32(own_epoch);
    out.write_u16(mtu);
    out.write_u16(config_hash);
    out.write_u16(token);
}

WireError ControlFrame::deserialize(BinDeserializer &in)
{
    if (in.remaining() < CONTROL_FRAME_SIZE)
    {
        return WireError::Truncated;
    }

    const u8 framing = in.read_u8();
    if (static_cast<u8>(framing >> VERSION_SHIFT) != VERSION)
    {
        return WireError::BadVersion;
    }
    if (static_cast<u8>(framing & FLAG_MASK) != FLAG_CONTROL)
    {
        return WireError::BadFlags;
    }

    const u8 type_value = in.read_u8();
    const u8 status_value = in.read_u8();
    if (!control_type_valid(type_value))
    {
        return WireError::BadType;
    }
    if (!control_status_valid(status_value))
    {
        return WireError::BadStatus;
    }

    const u32 peer = in.read_u32();
    const u32 own = in.read_u32();
    const u16 peer_mtu = in.read_u16();
    const u16 hash = in.read_u16();
    const u16 echo = in.read_u16();

    type = static_cast<ControlType>(type_value);
    status = static_cast<ControlStatus>(status_value);
    peer_epoch = peer;
    own_epoch = own;
    mtu = peer_mtu;
    config_hash = hash;
    token = echo;
    return WireError::Ok;
}

usize ControlFrame::serialized_size() const noexcept
{
    return CONTROL_FRAME_SIZE;
}
} // namespace meaproto
