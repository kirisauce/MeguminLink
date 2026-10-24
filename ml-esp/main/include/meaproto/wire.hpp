#pragma once
#ifndef __MEAPROTO_WIRE_HPP__
#define __MEAPROTO_WIRE_HPP__

#include "meaproto/types.hpp"
#include "meguminlink/binfmt.hpp"

#include <concepts>
#include <cstddef>
#include <span>

namespace meaproto
{
/** 协议版本：首字节高 4 位 */
inline constexpr u8 VERSION = 1;

inline constexpr u8 VERSION_SHIFT = 4;
inline constexpr u8 VERSION_MASK = 0x0F;
inline constexpr u8 CHANNEL_MASK = 0x0F;

/** datagram 头标志位 */
inline constexpr u8 FLAG_HAS_DATA = 0x01;
inline constexpr u8 FLAG_ACK_ONLY = 0x02;
inline constexpr u8 FLAG_CONTROL = 0x04;
inline constexpr u8 FLAG_NEEDS_ACK = 0x08;
inline constexpr u8 FLAG_MASK = 0x0F;

/** 消息头标志位 */
inline constexpr u8 MESSAGE_FLAG_FRAGMENTED = 0x01;

inline constexpr usize DATAGRAM_HEADER_SIZE = 11;
inline constexpr usize CONTROL_FRAME_SIZE = 17;
inline constexpr usize MESSAGE_HEADER_BASE_SIZE = 3;
inline constexpr usize MESSAGE_ORDERING_SIZE = 2;
inline constexpr usize MESSAGE_SPLIT_SIZE = 6;
inline constexpr usize ACK_BITS_WIDTH = 32;
inline constexpr u16 MIN_SPLIT_COUNT = 2;

enum class WireError : u8
{
    Ok,
    Truncated,
    BufferTooSmall,
    BadVersion,
    BadFlags,
    BadType,
    BadStatus,
    BadLength,
    BadCount,
};

enum class ControlType : u8
{
    ConnectRequest = 0,
    ConnectAccept = 1,
    ConnectReject = 2,
    Disconnect = 3,
    KeepAlive = 4,
    KeepAliveAck = 5,
    Ping = 6,
    Pong = 7,
};

enum class ControlStatus : u8
{
    Ok = 0,
    VersionMismatch = 1,
    ConfigMismatch = 2,
    Busy = 3,
    Timeout = 4,
};

struct DatagramHeader
{
    u8 version = VERSION;
    u8 flags = 0;
    u8 channel = 0;
    u16 datagram_seq = 0;
    u8 message_count = 0;
    u16 ack_seq = 0;
    u32 ack_bits = 0;

    void serialize(BinSerializer &out) const;
    WireError deserialize(BinDeserializer &in);
    usize serialized_size() const noexcept;
};

struct MessageHeader
{
    bool fragmented = false;
    u16 payload_len = 0;
    u16 ordering = 0;
    u16 split_id = 0;
    u16 split_index = 0;
    u16 split_count = 0;
    Reliability reliability = Reliability::ReliableOrdered; /**< 只在本地决定布局，不上线 */

    void serialize(BinSerializer &out) const;
    WireError deserialize(BinDeserializer &in);
    usize serialized_size() const noexcept;
};

struct ControlFrame
{
    ControlType type = ControlType::ConnectRequest;
    ControlStatus status = ControlStatus::Ok;
    u32 peer_epoch = 0;
    u32 own_epoch = 0;
    u16 mtu = 0;
    u16 config_hash = 0;
    u16 token = 0;

    void serialize(BinSerializer &out) const;
    WireError deserialize(BinDeserializer &in);
    usize serialized_size() const noexcept;
};

/** 能把自身编码进游标，并事先报出编码后的长度 */
template <typename T>
concept Encodable = requires(const T &value, BinSerializer &out) {
    { value.serialize(out) } -> std::same_as<void>;
    { value.serialized_size() } -> std::convertible_to<usize>;
};

/** 能从游标读回自身；越界或字段非法返回错误 */
template <typename T>
concept Decodable = requires(T &value, BinDeserializer &in) {
    { value.deserialize(in) } -> std::same_as<WireError>;
};

template <typename T>
concept WireMessage = Encodable<T> && Decodable<T>;

/** 唯一入口：按 serialized_size() 校验缓冲后写入 */
template <WireMessage T> WireError encode(const T &value, std::span<u8> out, usize &written) noexcept
{
    if (out.size() < value.serialized_size())
    {
        return WireError::BufferTooSmall;
    }

    BinSerializer serializer{out.data(), out.size()};
    value.serialize(serializer);
    written = serializer.size();
    return WireError::Ok;
}

/** 唯一入口：越界由各类型的 deserialize 判定 */
template <WireMessage T> WireError decode(std::span<const u8> in, T &value) noexcept
{
    BinDeserializer deserializer{in.data(), in.size()};
    return value.deserialize(deserializer);
}

} // namespace meaproto

#endif // __MEAPROTO_WIRE_HPP__
