// MeaProto 报文编解码（meaproto/wire.hpp）的宿主单元测试：往返、字段边界、
// 截断与非法字段的拒绝路径。用法见同目录 Makefile：cd ml-esp/test && make meaproto

#include "meaproto/hash.hpp"
#include "meaproto/wire.hpp"

#include <array>
#include <cstddef>
#include <print>
#include <source_location>
#include <span>

using namespace meaproto;

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool condition, std::source_location where = std::source_location::current())
{
    if (condition)
    {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::print("    [FAIL] {}:{}\n", where.file_name(), where.line());
}

template <typename Actual, typename Expected>
void check_eq(Actual actual, Expected expected, std::source_location where = std::source_location::current())
{
    const long long lhs = static_cast<long long>(actual);
    const long long rhs = static_cast<long long>(expected);
    if (lhs == rhs)
    {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::print("    [FAIL] {}:{}  got {}, want {}\n", where.file_name(), where.line(), lhs, rhs);
}

static_assert(Encodable<DatagramHeader>);
static_assert(Decodable<DatagramHeader>);
static_assert(WireMessage<MessageHeader>);
static_assert(WireMessage<ControlFrame>);
static_assert(!Encodable<int>);
static_assert(!Decodable<int>);

using Buffer = std::array<u8, 64>;

constexpr usize MESSAGE_HEADER_RELIABLE_SIZE = MESSAGE_HEADER_BASE_SIZE + MESSAGE_ORDERING_SIZE;
constexpr usize MESSAGE_HEADER_FRAGMENT_SIZE = MESSAGE_HEADER_BASE_SIZE + MESSAGE_ORDERING_SIZE + MESSAGE_SPLIT_SIZE;

template <WireMessage T> usize encode_into(const T &value, Buffer &buffer)
{
    usize written = 0;
    const WireError error = encode(value, std::span<u8>{buffer.data(), buffer.size()}, written);
    return error == WireError::Ok ? written : 0;
}

template <WireMessage T> WireError decode_from(const Buffer &buffer, usize length, T &value)
{
    return decode(std::span<const u8>{buffer.data(), length}, value);
}

DatagramHeader make_datagram()
{
    DatagramHeader header;
    header.flags = static_cast<u8>(FLAG_HAS_DATA | FLAG_NEEDS_ACK);
    header.channel = 3;
    header.datagram_seq = 0x1234;
    header.message_count = 1;
    header.ack_seq = 0x5678;
    header.ack_bits = 0xFFFF0000u;
    return header;
}

void check_datagram(const DatagramHeader &decoded, const DatagramHeader &expected)
{
    check_eq(decoded.version, VERSION);
    check_eq(decoded.flags, expected.flags);
    check_eq(decoded.channel, expected.channel);
    check_eq(decoded.datagram_seq, expected.datagram_seq);
    check_eq(decoded.message_count, expected.message_count);
    check_eq(decoded.ack_seq, expected.ack_seq);
    check_eq(decoded.ack_bits, expected.ack_bits);
}

void datagram_roundtrip()
{
    Buffer buffer{};
    const DatagramHeader header = make_datagram();
    const usize length = encode_into(header, buffer);
    check_eq(length, DATAGRAM_HEADER_SIZE);

    DatagramHeader decoded;
    check_eq(decode_from(buffer, length, decoded), WireError::Ok);
    check_datagram(decoded, header);
}

void datagram_roundtrip_edges()
{
    Buffer buffer{};
    DatagramHeader header;
    header.flags = FLAG_ACK_ONLY;
    header.channel = CHANNEL_MASK;
    header.datagram_seq = 0;
    header.message_count = 0;
    header.ack_seq = 0xFFFF;
    header.ack_bits = 0xFFFFFFFFu;

    const usize length = encode_into(header, buffer);
    check_eq(length, DATAGRAM_HEADER_SIZE);

    DatagramHeader decoded;
    check_eq(decode_from(buffer, length, decoded), WireError::Ok);
    check_datagram(decoded, header);

    header.flags = static_cast<u8>(FLAG_HAS_DATA | FLAG_NEEDS_ACK);
    header.datagram_seq = 0xFFFF;
    header.message_count = 0xFF;
    header.channel = 0;
    encode_into(header, buffer);
    check_eq(decode_from(buffer, DATAGRAM_HEADER_SIZE, decoded), WireError::Ok);
    check_datagram(decoded, header);
}

void datagram_truncated()
{
    Buffer buffer{};
    encode_into(make_datagram(), buffer);
    for (usize size = 0; size < DATAGRAM_HEADER_SIZE; ++size)
    {
        DatagramHeader decoded;
        check_eq(decode_from(buffer, size, decoded), WireError::Truncated);
    }
}

void datagram_bad_version()
{
    const u8 versions[] = {0, 2, 15};
    for (const u8 version : versions)
    {
        Buffer buffer{};
        encode_into(make_datagram(), buffer);
        buffer[0] = static_cast<u8>((version << VERSION_SHIFT) | (buffer[0] & FLAG_MASK));

        DatagramHeader decoded;
        check_eq(decode_from(buffer, DATAGRAM_HEADER_SIZE, decoded), WireError::BadVersion);
    }
}

void datagram_bad_flags()
{
    struct Case
    {
        u8 flags;
        u8 count;
    };
    const Case cases[] = {
        {FLAG_CONTROL, 0},
        {static_cast<u8>(FLAG_HAS_DATA | FLAG_ACK_ONLY), 1},
        {FLAG_ACK_ONLY, 3},
        {FLAG_HAS_DATA, 0},
        {0, 0},
    };

    for (const Case &item : cases)
    {
        Buffer buffer{};
        DatagramHeader header = make_datagram();
        header.flags = item.flags;
        header.message_count = item.count;
        encode_into(header, buffer);

        DatagramHeader decoded;
        check_eq(decode_from(buffer, DATAGRAM_HEADER_SIZE, decoded), WireError::BadFlags);
    }
}

void datagram_channel_reserved_ignored()
{
    Buffer buffer{};
    const DatagramHeader header = make_datagram();
    encode_into(header, buffer);
    buffer[1] = static_cast<u8>(buffer[1] | 0xF0);

    DatagramHeader decoded;
    check_eq(decode_from(buffer, DATAGRAM_HEADER_SIZE, decoded), WireError::Ok);
    check_eq(decoded.channel, header.channel);
}

void message_unreliable_roundtrip()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::Unreliable;
    header.payload_len = 0xFFFF;
    check_eq(encode_into(header, buffer), MESSAGE_HEADER_BASE_SIZE);

    MessageHeader decoded;
    decoded.reliability = Reliability::Unreliable;
    check_eq(decode_from(buffer, MESSAGE_HEADER_BASE_SIZE, decoded), WireError::Ok);
    check(!decoded.fragmented);
    check_eq(decoded.payload_len, 0xFFFF);
}

void message_reliable_roundtrip()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::ReliableOrdered;
    header.payload_len = 1274;
    header.ordering = 60000;
    check_eq(encode_into(header, buffer), MESSAGE_HEADER_RELIABLE_SIZE);

    MessageHeader decoded;
    decoded.reliability = Reliability::ReliableOrdered;
    check_eq(decode_from(buffer, MESSAGE_HEADER_RELIABLE_SIZE, decoded), WireError::Ok);
    check(!decoded.fragmented);
    check_eq(decoded.payload_len, 1274);
    check_eq(decoded.ordering, 60000);
}

void message_fragment_roundtrip()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::ReliableOrdered;
    header.fragmented = true;
    header.payload_len = 1448;
    header.ordering = 7;
    header.split_id = 9;
    header.split_index = 2;
    header.split_count = 3;
    check_eq(encode_into(header, buffer), MESSAGE_HEADER_FRAGMENT_SIZE);

    MessageHeader decoded;
    decoded.reliability = Reliability::ReliableOrdered;
    check_eq(decode_from(buffer, MESSAGE_HEADER_FRAGMENT_SIZE, decoded), WireError::Ok);
    check(decoded.fragmented);
    check_eq(decoded.payload_len, 1448);
    check_eq(decoded.ordering, 7);
    check_eq(decoded.split_id, 9);
    check_eq(decoded.split_index, 2);
    check_eq(decoded.split_count, 3);
}

void message_bad_flags()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::Unreliable;
    header.fragmented = true;
    encode_into(header, buffer);

    MessageHeader decoded;
    decoded.reliability = Reliability::Unreliable;
    check_eq(decode_from(buffer, MESSAGE_HEADER_BASE_SIZE, decoded), WireError::BadFlags);

    buffer[0] = 0x02;
    decoded.reliability = Reliability::ReliableOrdered;
    check_eq(decode_from(buffer, MESSAGE_HEADER_BASE_SIZE, decoded), WireError::BadFlags);
}

void message_bad_split()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::ReliableOrdered;
    header.fragmented = true;
    header.split_count = 1;
    header.split_index = 0;
    encode_into(header, buffer);

    MessageHeader decoded;
    decoded.reliability = Reliability::ReliableOrdered;
    check_eq(decode_from(buffer, MESSAGE_HEADER_FRAGMENT_SIZE, decoded), WireError::BadCount);

    header.split_count = 3;
    header.split_index = 3;
    encode_into(header, buffer);
    check_eq(decode_from(buffer, MESSAGE_HEADER_FRAGMENT_SIZE, decoded), WireError::BadLength);

    header.split_count = 0;
    header.split_index = 0;
    encode_into(header, buffer);
    check_eq(decode_from(buffer, MESSAGE_HEADER_FRAGMENT_SIZE, decoded), WireError::BadCount);
}

void message_truncated()
{
    Buffer buffer{};
    MessageHeader header;
    header.reliability = Reliability::ReliableOrdered;
    header.fragmented = true;
    header.split_count = 2;
    header.split_index = 1;
    encode_into(header, buffer);

    for (usize size = 0; size < MESSAGE_HEADER_FRAGMENT_SIZE; ++size)
    {
        MessageHeader decoded;
        decoded.reliability = Reliability::ReliableOrdered;
        check_eq(decode_from(buffer, size, decoded), WireError::Truncated);
    }
}

void control_roundtrip()
{
    for (u8 type = 0; type <= static_cast<u8>(ControlType::Pong); ++type)
    {
        for (u8 status = 0; status <= static_cast<u8>(ControlStatus::Timeout); ++status)
        {
            Buffer buffer{};
            ControlFrame frame;
            frame.type = static_cast<ControlType>(type);
            frame.status = static_cast<ControlStatus>(status);
            frame.peer_epoch = 0xDEADBEEFu;
            frame.own_epoch = 0x01020304u;
            frame.mtu = 1470;
            frame.config_hash = 0xA5A5;
            frame.token = 0xFFFF;
            check_eq(encode_into(frame, buffer), CONTROL_FRAME_SIZE);

            ControlFrame decoded;
            check_eq(decode_from(buffer, CONTROL_FRAME_SIZE, decoded), WireError::Ok);
            check_eq(static_cast<int>(decoded.type), static_cast<int>(frame.type));
            check_eq(static_cast<int>(decoded.status), static_cast<int>(frame.status));
            check_eq(decoded.peer_epoch, frame.peer_epoch);
            check_eq(decoded.own_epoch, frame.own_epoch);
            check_eq(decoded.mtu, frame.mtu);
            check_eq(decoded.config_hash, frame.config_hash);
            check_eq(decoded.token, frame.token);
        }
    }
}

void control_bad()
{
    Buffer buffer{};
    ControlFrame frame;
    frame.type = ControlType::Ping;
    frame.token = 42;
    encode_into(frame, buffer);

    for (usize size = 0; size < CONTROL_FRAME_SIZE; ++size)
    {
        ControlFrame decoded;
        check_eq(decode_from(buffer, size, decoded), WireError::Truncated);
    }

    ControlFrame decoded;
    Buffer bad_version = buffer;
    bad_version[0] = static_cast<u8>((2 << VERSION_SHIFT) | FLAG_CONTROL);
    check_eq(decode_from(bad_version, CONTROL_FRAME_SIZE, decoded), WireError::BadVersion);

    Buffer bad_flags = buffer;
    bad_flags[0] = static_cast<u8>((VERSION << VERSION_SHIFT) | FLAG_CONTROL | FLAG_HAS_DATA);
    check_eq(decode_from(bad_flags, CONTROL_FRAME_SIZE, decoded), WireError::BadFlags);

    Buffer bad_type = buffer;
    bad_type[1] = static_cast<u8>(static_cast<u8>(ControlType::Pong) + 1);
    check_eq(decode_from(bad_type, CONTROL_FRAME_SIZE, decoded), WireError::BadType);

    Buffer bad_status = buffer;
    bad_status[2] = static_cast<u8>(static_cast<u8>(ControlStatus::Timeout) + 1);
    check_eq(decode_from(bad_status, CONTROL_FRAME_SIZE, decoded), WireError::BadStatus);
}

void encode_buffer_too_small()
{
    const DatagramHeader header = make_datagram();
    std::array<u8, DATAGRAM_HEADER_SIZE - 1> small{};
    usize written = 0;
    check_eq(encode(header, std::span<u8>{small.data(), small.size()}, written), WireError::BufferTooSmall);
    check_eq(written, 0);
}

void config_hash_sensitive()
{
    const ChannelSpec base[] = {
        {ChannelId{1}, Reliability::ReliableOrdered, 1274, 8, 1, 8, 8},
        {ChannelId{2}, Reliability::ReliableOrdered, 512, 16, 3, 16, 8},
    };
    const u16 hash = config_hash(std::span<const ChannelSpec>{base, 2});
    check_eq(hash, config_hash(std::span<const ChannelSpec>{base, 2}));

    ChannelSpec changed[2] = {base[0], base[1]};
    changed[0].id = ChannelId{3};
    check(hash != config_hash(std::span<const ChannelSpec>{changed, 2}));

    changed[0] = base[0];
    changed[1].reliability = Reliability::Unreliable;
    check(hash != config_hash(std::span<const ChannelSpec>{changed, 2}));

    changed[1] = base[1];
    changed[0].max_payload = 1273;
    check(hash != config_hash(std::span<const ChannelSpec>{changed, 2}));

    const ChannelSpec swapped[] = {base[1], base[0]};
    check(hash != config_hash(std::span<const ChannelSpec>{swapped, 2}));

    check(hash != config_hash(std::span<const ChannelSpec>{}));
}

struct TestCase
{
    const char *name;
    void (*run)();
};

const TestCase TESTS[] = {
    {"datagram 往返", datagram_roundtrip},
    {"datagram 边界值", datagram_roundtrip_edges},
    {"datagram 截断", datagram_truncated},
    {"datagram 版本非法", datagram_bad_version},
    {"datagram flags 矛盾", datagram_bad_flags},
    {"datagram channel 保留位忽略", datagram_channel_reserved_ignored},
    {"message 不可靠往返", message_unreliable_roundtrip},
    {"message 可靠往返", message_reliable_roundtrip},
    {"message 分片往返", message_fragment_roundtrip},
    {"message flags 非法", message_bad_flags},
    {"message 分片字段非法", message_bad_split},
    {"message 截断", message_truncated},
    {"control 往返", control_roundtrip},
    {"control 非法字段", control_bad},
    {"encode 缓冲不足", encode_buffer_too_small},
    {"config_hash 敏感度", config_hash_sensitive},
};
} // namespace

int main()
{
    for (const TestCase &test : TESTS)
    {
        const int before = g_fail;
        std::print("== {}\n", test.name);
        test.run();
        std::print("   {}\n", (g_fail == before) ? "通过" : "失败");
    }

    std::print("\n断言：{} 通过 / {} 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
