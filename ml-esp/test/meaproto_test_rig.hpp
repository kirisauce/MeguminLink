// meaproto 宿主测试的公共夹具：假空口、假 Sink、通道夹具与有损双端。
#pragma once
#ifndef __MLINK_TEST_MEAPROTO_RIG_HPP__
#define __MLINK_TEST_MEAPROTO_RIG_HPP__

#include "meaproto/core.hpp"
#include "meaproto/hash.hpp"
#include "meaproto/wire.hpp"
#include "test_common.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace mptest
{
using namespace meaproto;
using test::check;
using test::check_eq;

inline constexpr u16 MTU = 1470;
inline constexpr u32 START_MS = 1000;
inline constexpr u32 FIRST_EPOCH = 0x01020304;
inline constexpr u32 PEER_EPOCH = 0x0A0B;
inline constexpr u16 PEER_MTU = 1400;
inline constexpr Config DEFAULT_CONFIG = {};

inline u16 empty_hash()
{
    return config_hash(std::span<const ChannelSpec>{});
}

struct FakeSink final : Sink
{
    void on_message(ChannelId channel, std::span<const u8> data) override
    {
        channels.push_back(channel);
        messages.emplace_back(data.begin(), data.end());
    }

    void on_connected() override
    {
        ++connected_count;
    }

    void on_disconnected(DisconnectReason reason) override
    {
        ++disconnected_count;
        last_reason = reason;
    }

    int connected_count = 0;
    int disconnected_count = 0;
    DisconnectReason last_reason = DisconnectReason::Requested;
    std::vector<ChannelId> channels;
    std::vector<std::vector<u8>> messages;
};

/** 假空口：记录发出的每一帧，可切换成发送失败 */
struct FakeLink
{
    static bool send(void *ctx, std::span<const u8> data)
    {
        auto &self = *static_cast<FakeLink *>(ctx);
        if (!self.accepting)
        {
            return false;
        }
        self.frames.emplace_back(data.begin(), data.end());
        return true;
    }

    static u32 random(void *ctx)
    {
        auto &self = *static_cast<FakeLink *>(ctx);
        return self.next_epoch++;
    }

    std::vector<std::vector<u8>> frames;
    bool accepting = true;
    u32 next_epoch = FIRST_EPOCH;
};

/** 空通道表的 Core：只走握手状态机 */
struct Rig
{
    Rig()
        : core(std::span<const ChannelSpec>{}, DEFAULT_CONFIG, MTU, sink,
               CoreHooks{&link, &FakeLink::send, &FakeLink::random}, {})
    {
    }

    FakeLink link;
    FakeSink sink;
    Core core;
    u32 now = START_MS;
};

inline ControlFrame decoded(const std::vector<u8> &bytes)
{
    ControlFrame frame;
    const WireError error = decode(std::span<const u8>{bytes.data(), bytes.size()}, frame);
    check_eq(error, WireError::Ok);
    return frame;
}

/** 把控制帧编成字节；version 用于构造版本不符的帧 */
inline std::vector<u8> encode_control(const ControlFrame &frame, u8 version = VERSION)
{
    u8 buffer[CONTROL_FRAME_SIZE] = {};
    usize written = 0;
    check_eq(encode(frame, std::span<u8>{buffer, sizeof(buffer)}, written), WireError::Ok);
    buffer[0] = static_cast<u8>((version << VERSION_SHIFT) | FLAG_CONTROL);
    return std::vector<u8>{buffer, buffer + written};
}

inline void deliver(Rig &rig, const ControlFrame &frame, u8 version = VERSION)
{
    const std::vector<u8> bytes = encode_control(frame, version);
    rig.core.receive(rig.now, std::span<const u8>{bytes.data(), bytes.size()});
}

inline ControlFrame last_sent(const Rig &rig)
{
    return decoded(rig.link.frames.back());
}

inline ControlFrame connect_request(u32 own_epoch, u16 mtu, u16 hash)
{
    ControlFrame frame;
    frame.type = ControlType::ConnectRequest;
    frame.own_epoch = own_epoch;
    frame.mtu = mtu;
    frame.config_hash = hash;
    return frame;
}

inline ControlFrame connect_accept(u32 own_epoch, u32 peer_epoch, u16 mtu)
{
    ControlFrame frame;
    frame.type = ControlType::ConnectAccept;
    frame.own_epoch = own_epoch;
    frame.peer_epoch = peer_epoch;
    frame.mtu = mtu;
    frame.config_hash = empty_hash();
    return frame;
}

inline ControlFrame disconnect(u32 own_epoch)
{
    ControlFrame frame;
    frame.type = ControlType::Disconnect;
    frame.own_epoch = own_epoch;
    frame.config_hash = empty_hash();
    return frame;
}

// ---- 通道夹具 ----

inline constexpr ChannelId CH_A{1};
inline constexpr ChannelId CH_B{2};
inline constexpr ChannelId CH_C{3};
inline constexpr ChannelId CH_W{4};
inline constexpr ChannelId CH_R{5};
inline constexpr ChannelId CH_S{6};

// clang-format off
/** 通道表：A 优先级 2、B 队列 1、C 不可靠、W 窗口 1、R 重排 1、S 大容量 */
inline constexpr ChannelSpec TEST_CHANNELS[] = {
    {CH_A, Reliability::ReliableOrdered, 64, 2, 2, 2, 2},
    {CH_B, Reliability::ReliableOrdered, 32, 1, 1, 1, 1},
    {CH_C, Reliability::Unreliable, 32, 2, 3, 2, 0},
    {CH_W, Reliability::ReliableOrdered, 32, 2, 4, 2, 1},
    {CH_R, Reliability::ReliableOrdered, 32, 3, 5, 1, 4},
    {CH_S, Reliability::ReliableOrdered, 32, 128, 6, 64, 8},
};
// clang-format on

/** 带通道表的 Rig：存储按通道表算出，握手后进入 Connected */
struct ChannelRig
{
    explicit ChannelRig(const Config &config = DEFAULT_CONFIG) : ChannelRig(TEST_CHANNELS, config)
    {
    }

    ChannelRig(std::span<const ChannelSpec> channels, const Config &config = DEFAULT_CONFIG)
        : storage(Core::storage_bytes(channels, config, MTU)),
          core(channels, config, MTU, sink, CoreHooks{&link, &FakeLink::send, &FakeLink::random}, storage)
    {
    }

    void connect()
    {
        core.open(now);
        const ControlFrame accept = connect_accept(PEER_EPOCH, decoded(link.frames.back()).own_epoch, PEER_MTU);
        const std::vector<u8> bytes = encode_control(accept);
        core.receive(now, std::span<const u8>{bytes.data(), bytes.size()});
    }

    FakeLink link;
    FakeSink sink;
    std::vector<u8> storage;
    Core core;
    u32 now = START_MS;
};

inline DatagramHeader datagram_header(const std::vector<u8> &bytes)
{
    DatagramHeader header;
    check_eq(decode(std::span<const u8>{bytes.data(), bytes.size()}, header), WireError::Ok);
    return header;
}

/** 造一个只带 ACK 字段的数据报，喂给 Core 释放发送窗口 */
inline void deliver_ack(Core &core, u32 now, u8 channel, u16 ack_seq, u32 ack_bits)
{
    DatagramHeader header;
    header.flags = FLAG_ACK_ONLY;
    header.channel = channel;
    header.ack_seq = ack_seq;
    header.ack_bits = ack_bits;
    header.message_count = 0;

    u8 buffer[32] = {};
    usize written = 0;
    check_eq(encode(header, std::span<u8>{buffer, sizeof(buffer)}, written), WireError::Ok);
    core.receive(now, std::span<const u8>{buffer, written});
}

/** 一对已连接的 Core：把一方发出的帧喂给另一方，可注入丢包与重复 */
struct Duo
{
    explicit Duo(std::span<const ChannelSpec> channels = TEST_CHANNELS, const Config &config = DEFAULT_CONFIG)
        : a(channels, config), b(channels, config)
    {
    }

    void connect()
    {
        a.core.open(a.now);
        flush(a, b);
        flush(b, a);
        check(a.core.connected());
        check(b.core.connected());
    }

    /** 有损空口：按百分比注入丢包与重复，伪随机但种子固定，可复现 */
    void flush(ChannelRig &from, ChannelRig &to)
    {
        for (const std::vector<u8> &frame : from.link.frames)
        {
            if (roll(drop_percent))
            {
                continue;
            }
            to.core.receive(to.now, std::span<const u8>{frame.data(), frame.size()});
            if (roll(duplicate_percent))
            {
                to.core.receive(to.now, std::span<const u8>{frame.data(), frame.size()});
            }
        }
        from.link.frames.clear();
    }

    /** 按百分比抽一次 */
    bool roll(int percent)
    {
        if (percent <= 0)
        {
            return false;
        }
        rng = rng * 1664525u + 1013904223u;
        return static_cast<int>((rng >> 16) % 100u) < percent;
    }

    void step(u32 delta)
    {
        a.now += delta;
        b.now += delta;
        a.core.step(a.now);
        b.core.step(b.now);
    }

    ChannelRig a;
    ChannelRig b;
    int drop_percent = 0;
    int duplicate_percent = 0;
    u32 rng = 12345;
};
} // namespace mptest

#endif // __MLINK_TEST_MEAPROTO_RIG_HPP__
