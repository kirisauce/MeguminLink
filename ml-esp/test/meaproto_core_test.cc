// Core 连接状态机的宿主单元测试：假时钟与假空口，不依赖 RTOS。
// 用法见同目录 Makefile：cd ml-esp/test && make meaproto-core

#include "meaproto/core.hpp"
#include "meaproto/hash.hpp"
#include "meaproto/wire.hpp"
#include "test_common.hpp"

#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

using namespace meaproto;

namespace
{
using test::check;
using test::check_eq;

constexpr u16 MTU = 1470;
constexpr u32 START_MS = 1000;
constexpr u32 FIRST_EPOCH = 0x01020304;
constexpr u32 PEER_EPOCH = 0x0A0B;
constexpr u16 PEER_MTU = 1400;
constexpr Config DEFAULT_CONFIG = {};

u16 empty_hash()
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

ControlFrame decoded(const std::vector<u8> &bytes)
{
    ControlFrame frame;
    const WireError error = decode(std::span<const u8>{bytes.data(), bytes.size()}, frame);
    check_eq(error, WireError::Ok);
    return frame;
}

/** 把控制帧编成字节；version 用于构造版本不符的帧 */
std::vector<u8> encode_control(const ControlFrame &frame, u8 version = VERSION)
{
    u8 buffer[CONTROL_FRAME_SIZE] = {};
    usize written = 0;
    check_eq(encode(frame, std::span<u8>{buffer, sizeof(buffer)}, written), WireError::Ok);
    buffer[0] = static_cast<u8>((version << VERSION_SHIFT) | FLAG_CONTROL);
    return std::vector<u8>{buffer, buffer + written};
}

void deliver(Rig &rig, const ControlFrame &frame, u8 version = VERSION)
{
    const std::vector<u8> bytes = encode_control(frame, version);
    rig.core.receive(rig.now, std::span<const u8>{bytes.data(), bytes.size()});
}

ControlFrame last_sent(const Rig &rig)
{
    return decoded(rig.link.frames.back());
}

ControlFrame connect_request(u32 own_epoch, u16 mtu, u16 hash)
{
    ControlFrame frame;
    frame.type = ControlType::ConnectRequest;
    frame.own_epoch = own_epoch;
    frame.mtu = mtu;
    frame.config_hash = hash;
    return frame;
}

ControlFrame connect_accept(u32 own_epoch, u32 peer_epoch, u16 mtu)
{
    ControlFrame frame;
    frame.type = ControlType::ConnectAccept;
    frame.own_epoch = own_epoch;
    frame.peer_epoch = peer_epoch;
    frame.mtu = mtu;
    frame.config_hash = empty_hash();
    return frame;
}

ControlFrame disconnect(u32 own_epoch)
{
    ControlFrame frame;
    frame.type = ControlType::Disconnect;
    frame.own_epoch = own_epoch;
    frame.config_hash = empty_hash();
    return frame;
}

/** 走完 open() + ConnectAccept，进入 Connected */
void handshake(Rig &rig)
{
    rig.core.open(rig.now);
    const u32 own_epoch = last_sent(rig).own_epoch;
    deliver(rig, connect_accept(PEER_EPOCH, own_epoch, PEER_MTU));
}

void idle_initial()
{
    Rig rig;
    check(rig.core.state() == Core::State::Idle);
    check(!rig.core.connected());
    check(!rig.core.connecting());
    check(rig.link.frames.empty());
    check_eq(rig.core.step(rig.now), rig.now + 1000);
    check_eq(rig.core.mtu(), MTU);
}

void open_sends_connect_request()
{
    Rig rig;
    rig.core.open(rig.now);

    check(rig.core.connecting());
    check_eq(rig.link.frames.size(), 1);
    const std::vector<u8> &bytes = rig.link.frames.front();
    check_eq(bytes.size(), CONTROL_FRAME_SIZE);
    check_eq(bytes[0], static_cast<u8>((VERSION << VERSION_SHIFT) | FLAG_CONTROL));

    const ControlFrame frame = decoded(bytes);
    check_eq(frame.type, ControlType::ConnectRequest);
    check_eq(frame.status, ControlStatus::Ok);
    check_eq(frame.peer_epoch, 0);
    check_eq(frame.own_epoch, FIRST_EPOCH);
    check_eq(frame.mtu, MTU);
    check_eq(frame.config_hash, empty_hash());
    check_eq(frame.token, 0);
}

void open_idempotent()
{
    Rig rig;
    rig.core.open(rig.now);
    rig.core.open(rig.now);
    rig.core.open(rig.now);
    check_eq(rig.link.frames.size(), 1);
    check(rig.core.connecting());
}

void close_sends_disconnect()
{
    Rig rig;
    rig.core.open(rig.now);
    rig.core.close(rig.now);

    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.link.frames.size(), 2);
    const ControlFrame frame = last_sent(rig);
    check_eq(frame.type, ControlType::Disconnect);
    check_eq(frame.peer_epoch, 0);
    check_eq(rig.sink.disconnected_count, 1);
    check(rig.sink.last_reason == DisconnectReason::Requested);
}

void close_when_idle_is_noop()
{
    Rig rig;
    rig.core.close(rig.now);
    check(rig.link.frames.empty());
    check_eq(rig.sink.disconnected_count, 0);
    check(rig.core.state() == Core::State::Idle);
}

void send_failure_keeps_connecting()
{
    Rig rig;
    rig.link.accepting = false;
    rig.core.open(rig.now);
    check(rig.core.connecting());
    check(rig.link.frames.empty());
}

void snapshot_reflects_state()
{
    Rig rig;
    const Stats idle = rig.core.snapshot();
    check(!idle.connected);
    check_eq(idle.peer_epoch, 0);

    rig.core.open(rig.now);
    const Stats connecting = rig.core.snapshot();
    check(!connecting.connected);
    check_eq(connecting.peer_epoch, 0);
}

void accept_connect_request()
{
    Rig rig;
    deliver(rig, connect_request(PEER_EPOCH, PEER_MTU, empty_hash()));

    check(rig.core.connected());
    check_eq(rig.sink.connected_count, 1);
    check_eq(rig.core.mtu(), PEER_MTU);
    check_eq(rig.link.frames.size(), 1);

    const ControlFrame reply = last_sent(rig);
    check_eq(reply.type, ControlType::ConnectAccept);
    check_eq(reply.status, ControlStatus::Ok);
    check_eq(reply.peer_epoch, PEER_EPOCH);
    check(reply.own_epoch != 0);
}

void reject_config_mismatch()
{
    Rig rig;
    deliver(rig, connect_request(PEER_EPOCH, PEER_MTU, static_cast<u16>(empty_hash() ^ 0x1234u)));

    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.sink.connected_count, 0);
    const ControlFrame reply = last_sent(rig);
    check_eq(reply.type, ControlType::ConnectReject);
    check_eq(reply.status, ControlStatus::ConfigMismatch);
}

void reject_bad_version()
{
    Rig rig;
    deliver(rig, connect_request(PEER_EPOCH, PEER_MTU, empty_hash()), 2);

    check(rig.core.state() == Core::State::Idle);
    const ControlFrame reply = last_sent(rig);
    check_eq(reply.type, ControlType::ConnectReject);
    check_eq(reply.status, ControlStatus::VersionMismatch);
}

void ignore_bad_version_non_request()
{
    Rig rig;
    ControlFrame keepalive;
    keepalive.type = ControlType::KeepAlive;
    deliver(rig, keepalive, 2);
    check(rig.link.frames.empty());
}

void complete_handshake_with_accept()
{
    Rig rig;
    handshake(rig);

    check(rig.core.connected());
    check_eq(rig.sink.connected_count, 1);
    check_eq(rig.core.mtu(), PEER_MTU);
    check_eq(rig.core.snapshot().peer_epoch, PEER_EPOCH);
}

void stale_accept_ignored()
{
    Rig rig;
    rig.core.open(rig.now);
    const u32 own_epoch = last_sent(rig).own_epoch;
    deliver(rig, connect_accept(PEER_EPOCH, own_epoch + 1, PEER_MTU));

    check(rig.core.connecting());
    check_eq(rig.sink.connected_count, 0);
}

void peer_reject_returns_to_idle()
{
    Rig rig;
    rig.core.open(rig.now);
    const u32 own_epoch = last_sent(rig).own_epoch;

    ControlFrame reject;
    reject.type = ControlType::ConnectReject;
    reject.status = ControlStatus::ConfigMismatch;
    reject.peer_epoch = own_epoch;
    deliver(rig, reject);

    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.sink.disconnected_count, 1);
    check(rig.sink.last_reason == DisconnectReason::Rejected);
}

void retry_resends_request()
{
    Rig rig;
    rig.core.open(rig.now);
    check_eq(rig.link.frames.size(), 1);

    rig.now += DEFAULT_CONFIG.connect_retry_ms;
    rig.core.step(rig.now);

    check(rig.core.connecting());
    check_eq(rig.link.frames.size(), 2);
    check_eq(last_sent(rig).type, ControlType::ConnectRequest);
}

void retries_exhausted()
{
    Rig rig;
    rig.core.open(rig.now);
    for (int i = 0; i < 2 * DEFAULT_CONFIG.connect_max_retries && rig.core.connecting(); ++i)
    {
        rig.now += DEFAULT_CONFIG.connect_retry_ms;
        rig.core.step(rig.now);
    }

    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.sink.disconnected_count, 1);
    check(rig.sink.last_reason == DisconnectReason::Rejected);
    check_eq(rig.link.frames.size(), 1 + DEFAULT_CONFIG.connect_max_retries);
}

void open_throttled_after_failure()
{
    Rig rig;
    rig.core.open(rig.now);
    for (int i = 0; i < 2 * DEFAULT_CONFIG.connect_max_retries && rig.core.connecting(); ++i)
    {
        rig.now += DEFAULT_CONFIG.connect_retry_ms;
        rig.core.step(rig.now);
    }

    const std::size_t frames = rig.link.frames.size();
    rig.core.open(rig.now);
    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.link.frames.size(), frames);

    rig.now += DEFAULT_CONFIG.connect_retry_ms;
    rig.core.open(rig.now);
    check(rig.core.connecting());
    check_eq(rig.link.frames.size(), frames + 1);
}

void peer_disconnect_returns_to_idle()
{
    Rig rig;
    handshake(rig);
    deliver(rig, disconnect(PEER_EPOCH));

    check(rig.core.state() == Core::State::Idle);
    check_eq(rig.sink.disconnected_count, 1);
    check(rig.sink.last_reason == DisconnectReason::Requested);
}

void stale_disconnect_ignored()
{
    Rig rig;
    handshake(rig);
    deliver(rig, disconnect(PEER_EPOCH + 1));

    check(rig.core.connected());
    check_eq(rig.sink.disconnected_count, 0);
}

void duplicate_request_repeats_accept()
{
    Rig rig;
    handshake(rig);
    const std::size_t frames = rig.link.frames.size();
    deliver(rig, connect_request(PEER_EPOCH, PEER_MTU, empty_hash()));

    check(rig.core.connected());
    check_eq(rig.sink.connected_count, 1);
    check_eq(rig.link.frames.size(), frames + 1);
    check_eq(last_sent(rig).type, ControlType::ConnectAccept);
}

void restart_with_new_epoch()
{
    Rig rig;
    handshake(rig);
    const std::size_t frames = rig.link.frames.size();
    deliver(rig, connect_request(PEER_EPOCH + 7, PEER_MTU, empty_hash()));

    check(rig.core.connected());
    check_eq(rig.sink.connected_count, 1);
    check_eq(rig.core.snapshot().peer_epoch, PEER_EPOCH + 7);
    check_eq(rig.link.frames.size(), frames + 1);
    check_eq(last_sent(rig).type, ControlType::ConnectAccept);
    check_eq(last_sent(rig).peer_epoch, PEER_EPOCH + 7);
}

// ---- 通道发送队列 ----

constexpr ChannelId CH_A{1};
constexpr ChannelId CH_B{2};
constexpr ChannelId CH_C{3};
constexpr ChannelId CH_W{4};
constexpr ChannelId CH_R{5};

/** 通道表：A 优先级 2、B 优先级 1（队列 1）、C 不可靠、W 窗口 1、R 重排 1（队列 3） */
constexpr ChannelSpec TEST_CHANNELS[] = {
    {CH_A, Reliability::ReliableOrdered, 64, 2, 2, 2, 2},
    {CH_B, Reliability::ReliableOrdered, 32, 1, 1, 1, 1},
    {CH_C, Reliability::Unreliable, 32, 2, 3, 2, 0},
    {CH_W, Reliability::ReliableOrdered, 32, 2, 4, 2, 1},
    {CH_R, Reliability::ReliableOrdered, 32, 3, 5, 1, 4},
};

/** 带通道表的 Rig：存储按通道表算出，握手后进入 Connected */
struct ChannelRig
{
    explicit ChannelRig(const Config &config = DEFAULT_CONFIG)
        : storage(Core::storage_bytes(TEST_CHANNELS, config, MTU)),
          core(TEST_CHANNELS, config, MTU, sink, CoreHooks{&link, &FakeLink::send, &FakeLink::random}, storage)
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

DatagramHeader datagram_header(const std::vector<u8> &bytes)
{
    DatagramHeader header;
    check_eq(decode(std::span<const u8>{bytes.data(), bytes.size()}, header), WireError::Ok);
    return header;
}

/** 造一个只带 ACK 字段的数据报，喂给 Core 释放发送窗口 */
void deliver_ack(Core &core, u32 now, u8 channel, u16 ack_seq, u32 ack_bits)
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

void datagram_sent_for_queued_message()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[4] = {1, 2, 3, 4};

    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(rig.core.send(CH_A, payload) == SendResult::Ok);

    const std::size_t before = rig.link.frames.size();
    rig.core.step(rig.now);

    check_eq(rig.link.frames.size(), before + 2);
    const DatagramHeader first = datagram_header(rig.link.frames[before]);
    check_eq(first.channel, CH_A.value);
    check((first.flags & FLAG_NEEDS_ACK) != 0);
    check_eq(first.datagram_seq, 0);
    check_eq(rig.core.snapshot().datagrams_in_flight, 2);
}

void window_blocks_until_ack()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[4] = {};

    check(rig.core.send(CH_W, payload) == SendResult::Ok);
    check(rig.core.send(CH_W, payload) == SendResult::Ok);

    const std::size_t before = rig.link.frames.size();
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), before + 1); // 窗口只有 1
    check_eq(rig.core.snapshot().datagrams_in_flight, 1);

    deliver_ack(rig.core, rig.now, CH_W.value, 1, 0);
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), before + 2);
    check_eq(rig.core.snapshot().datagrams_in_flight, 1);
}

void retransmit_after_timeout()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[3] = {9, 8, 7};

    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    rig.core.step(rig.now);

    const std::vector<u8> sent = rig.link.frames.back();
    const std::size_t after_first = rig.link.frames.size();

    rig.now += DEFAULT_CONFIG.rto_initial_ms;
    rig.core.step(rig.now);

    check_eq(rig.link.frames.size(), after_first + 1);
    check(rig.link.frames.back() == sent);
    check_eq(rig.core.snapshot().datagrams_retransmitted, 1);
}

void ack_bits_release_slots()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[2] = {};

    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    rig.core.step(rig.now);
    check_eq(rig.core.snapshot().datagrams_in_flight, 2);

    deliver_ack(rig.core, rig.now, CH_A.value, 0, 0x1); // 只确认 seq 1
    check_eq(rig.core.snapshot().datagrams_in_flight, 1);

    deliver_ack(rig.core, rig.now, CH_A.value, 2, 0); // seq 0 也被覆盖
    check_eq(rig.core.snapshot().datagrams_in_flight, 0);
}

void quota_limits_frames_per_tick()
{
    ChannelRig rig(Config{.send_quota_per_tick = 1});
    rig.connect();
    const u8 payload[2] = {};

    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(rig.core.send(CH_A, payload) == SendResult::Ok);

    const std::size_t before = rig.link.frames.size();
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), before + 1);

    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), before + 2);
}

void priority_orders_channels()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[2] = {};

    check(rig.core.send(CH_A, payload) == SendResult::Ok); // 优先级 2
    check(rig.core.send(CH_B, payload) == SendResult::Ok); // 优先级 1

    const std::size_t before = rig.link.frames.size();
    rig.core.step(rig.now);

    check(rig.link.frames.size() >= before + 1);
    check_eq(datagram_header(rig.link.frames[before]).channel, CH_B.value);
}

void unreliable_channel_skips_window()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[2] = {};

    check(rig.core.send(CH_C, payload) == SendResult::Ok);
    check(rig.core.send(CH_C, payload) == SendResult::Ok);

    const std::size_t before = rig.link.frames.size();
    rig.core.step(rig.now);

    check_eq(rig.link.frames.size(), before + 2);
    const DatagramHeader header = datagram_header(rig.link.frames[before]);
    check_eq(static_cast<u8>(header.flags & FLAG_NEEDS_ACK), 0);
    check_eq(rig.core.snapshot().datagrams_in_flight, 0);
}

// ---- 接收侧：去重、排序与延迟确认 ----

/** 一对已连接的 Core：把一方发出的帧喂给另一方 */
struct Duo
{
    void connect()
    {
        a.core.open(a.now);
        flush(a, b);
        flush(b, a);
        check(a.core.connected());
        check(b.core.connected());
    }

    void flush(ChannelRig &from, ChannelRig &to)
    {
        for (const std::vector<u8> &frame : from.link.frames)
        {
            to.core.receive(to.now, std::span<const u8>{frame.data(), frame.size()});
        }
        from.link.frames.clear();
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
};

void receive_in_order()
{
    Duo duo;
    duo.connect();

    const u8 payload[3] = {7, 8, 9};
    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    duo.flush(duo.a, duo.b);

    check_eq(duo.b.sink.messages.size(), 1);
    check(duo.b.sink.messages[0] == std::vector<u8>{7, 8, 9});
    check(duo.b.sink.channels[0] == CH_A);
}

void receive_duplicate_once()
{
    Duo duo;
    duo.connect();

    const u8 payload[2] = {1, 1};
    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    const std::vector<u8> frame = duo.a.link.frames.back();
    duo.a.link.frames.clear();

    duo.b.core.receive(duo.b.now, std::span<const u8>{frame.data(), frame.size()});
    duo.b.core.receive(duo.b.now, std::span<const u8>{frame.data(), frame.size()});

    check_eq(duo.b.sink.messages.size(), 1);
    check_eq(duo.b.core.snapshot().duplicate_datagrams, 1);
}

void receive_out_of_order()
{
    Duo duo;
    duo.connect();

    const u8 first[1] = {1};
    const u8 second[1] = {2};
    check(duo.a.core.send(CH_A, first) == SendResult::Ok);
    check(duo.a.core.send(CH_A, second) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 2);
    const std::vector<u8> f0 = duo.a.link.frames[0];
    const std::vector<u8> f1 = duo.a.link.frames[1];
    duo.a.link.frames.clear();

    duo.b.core.receive(duo.b.now, std::span<const u8>{f1.data(), f1.size()});
    check_eq(duo.b.sink.messages.size(), 0); // 乱序的先压着重排

    duo.b.core.receive(duo.b.now, std::span<const u8>{f0.data(), f0.size()});
    check_eq(duo.b.sink.messages.size(), 2);
    check(duo.b.sink.messages[0] == std::vector<u8>{1});
    check(duo.b.sink.messages[1] == std::vector<u8>{2});
}

void receive_out_of_window()
{
    Duo duo;
    duo.connect();

    DatagramHeader header;
    header.flags = FLAG_ACK_ONLY | FLAG_NEEDS_ACK;
    header.channel = CH_A.value;
    header.datagram_seq = 100;
    header.message_count = 0;

    u8 buffer[32] = {};
    usize written = 0;
    check_eq(encode(header, std::span<u8>{buffer, sizeof(buffer)}, written), WireError::Ok);
    duo.b.core.receive(duo.b.now, std::span<const u8>{buffer, written});

    check_eq(duo.b.core.snapshot().out_of_window, 1);
    check_eq(duo.b.sink.messages.size(), 0);
}

void delayed_ack_releases_window()
{
    Duo duo;
    duo.connect();

    const u8 payload[2] = {};
    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    duo.flush(duo.a, duo.b);
    check_eq(duo.a.core.snapshot().datagrams_in_flight, 1);

    const std::size_t before = duo.b.link.frames.size();
    duo.b.core.step(duo.b.now); // 还没到 ack_delay
    check_eq(duo.b.link.frames.size(), before);

    duo.b.now += DEFAULT_CONFIG.ack_delay_ms;
    duo.b.core.step(duo.b.now);
    check_eq(duo.b.link.frames.size(), before + 1);

    duo.flush(duo.b, duo.a);
    check_eq(duo.a.core.snapshot().datagrams_in_flight, 0);
}

void reorder_overflow_keeps_unacked()
{
    Duo duo;
    duo.connect();

    const u8 first[1] = {1};
    const u8 second[1] = {2};
    const u8 third[1] = {3};
    check(duo.a.core.send(CH_R, first) == SendResult::Ok);
    check(duo.a.core.send(CH_R, second) == SendResult::Ok);
    check(duo.a.core.send(CH_R, third) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    check_eq(duo.a.link.frames.size(), 3);

    const std::vector<u8> f0 = duo.a.link.frames[0];
    const std::vector<u8> f1 = duo.a.link.frames[1];
    const std::vector<u8> f2 = duo.a.link.frames[2];
    duo.a.link.frames.clear();

    duo.b.core.receive(duo.b.now, std::span<const u8>{f2.data(), f2.size()}); // 进重排
    check_eq(duo.b.core.snapshot().reorder_overflow, 0);

    duo.b.core.receive(duo.b.now, std::span<const u8>{f1.data(), f1.size()}); // 重排满
    check_eq(duo.b.core.snapshot().reorder_overflow, 1);
    check_eq(duo.b.sink.messages.size(), 0);

    duo.b.core.receive(duo.b.now, std::span<const u8>{f0.data(), f0.size()});
    check_eq(duo.b.sink.messages.size(), 1);
    check(duo.b.sink.messages[0] == std::vector<u8>{1});
}

// ---- 可靠性切换 ----

void set_reliability_requires_connection()
{
    ChannelRig rig;
    check(!rig.core.set_reliability(CH_A, Reliability::Unreliable));

    rig.connect();
    check(rig.core.set_reliability(CH_A, Reliability::Unreliable));
    check(!rig.core.set_reliability(ChannelId{9}, Reliability::Unreliable));
}

void set_reliability_clears_state()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[2] = {};

    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    rig.core.step(rig.now);
    check_eq(rig.core.snapshot().datagrams_in_flight, 2);

    check(rig.core.set_reliability(CH_A, Reliability::Unreliable));
    check_eq(rig.core.snapshot().datagrams_in_flight, 0);
    check(rig.core.can_send(CH_A));
}

void set_reliability_restarts_ordering()
{
    Duo duo;
    duo.connect();

    const u8 first[1] = {1};
    check(duo.a.core.send(CH_A, first) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    duo.flush(duo.a, duo.b);
    check_eq(duo.b.sink.messages.size(), 1);

    check(duo.a.core.set_reliability(CH_A, Reliability::Unreliable));
    check(duo.b.core.set_reliability(CH_A, Reliability::Unreliable));

    check(duo.a.core.send(CH_A, first) == SendResult::Ok);
    const std::size_t before = duo.a.link.frames.size();
    duo.a.core.step(duo.a.now);
    check_eq(duo.a.link.frames.size(), before + 1);

    duo.flush(duo.a, duo.b);
    check_eq(duo.b.sink.messages.size(), 2);
}

void reconnect_resets_channels()
{
    Duo duo;
    duo.connect();

    const u8 payload[2] = {4, 2};
    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    check_eq(datagram_header(duo.a.link.frames.back()).datagram_seq, 0);
    duo.a.link.frames.clear();

    duo.a.core.close(duo.a.now);
    duo.flush(duo.a, duo.b);
    duo.flush(duo.b, duo.a);
    check(!duo.a.core.connected());

    duo.a.core.open(duo.a.now);
    duo.flush(duo.a, duo.b);
    duo.flush(duo.b, duo.a);
    check(duo.a.core.connected());
    check(duo.b.core.connected());

    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);
    check_eq(datagram_header(duo.a.link.frames.back()).datagram_seq, 0);

    duo.flush(duo.a, duo.b);
    check_eq(duo.b.sink.messages.size(), 1);
}

void send_not_connected()
{
    ChannelRig rig;
    const u8 payload[4] = {1, 2, 3, 4};
    check(rig.core.send(CH_A, payload) == SendResult::NotConnected);
}

void send_invalid_channel()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[1] = {0};
    check(rig.core.send(ChannelId{9}, payload) == SendResult::InvalidChannel);
    check(!rig.core.can_send(ChannelId{9}));
}

void send_too_large()
{
    ChannelRig rig;
    rig.connect();
    std::vector<u8> too_large(65, 0); // CH_A 上限 64
    check(rig.core.send(CH_A, too_large) == SendResult::TooLarge);

    std::vector<u8> fits(64, 0);
    check(rig.core.send(CH_A, fits) == SendResult::Ok);
}

void send_queue_depth()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[4] = {};

    check(rig.core.can_send(CH_A));
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    check(!rig.core.can_send(CH_A));
    check(rig.core.send(CH_A, payload) == SendResult::Busy);
}

void send_channel_isolation()
{
    ChannelRig rig;
    rig.connect();
    const u8 payload[8] = {};

    check(rig.core.send(CH_B, payload) == SendResult::Ok); // CH_B 深度 1，已满
    check(rig.core.send(CH_B, payload) == SendResult::Busy);
    check(rig.core.can_send(CH_A)); // 另一条通道照常
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
}

const test::Case TESTS[] = {
    {"initial state", idle_initial},
    {"open sends connect request", open_sends_connect_request},
    {"open is idempotent", open_idempotent},
    {"close sends disconnect", close_sends_disconnect},
    {"close on idle is a no-op", close_when_idle_is_noop},
    {"send failure keeps connecting", send_failure_keeps_connecting},
    {"snapshot reflects state", snapshot_reflects_state},
    {"accept connect request", accept_connect_request},
    {"reject config mismatch", reject_config_mismatch},
    {"reject bad version", reject_bad_version},
    {"ignore bad version non-request", ignore_bad_version_non_request},
    {"complete handshake with accept", complete_handshake_with_accept},
    {"stale accept ignored", stale_accept_ignored},
    {"peer reject returns to idle", peer_reject_returns_to_idle},
    {"retry resends request", retry_resends_request},
    {"retries exhausted", retries_exhausted},
    {"open throttled after failure", open_throttled_after_failure},
    {"peer disconnect returns to idle", peer_disconnect_returns_to_idle},
    {"stale disconnect ignored", stale_disconnect_ignored},
    {"duplicate request repeats accept", duplicate_request_repeats_accept},
    {"restart with new epoch", restart_with_new_epoch},
    {"send requires connection", send_not_connected},
    {"send rejects invalid channel", send_invalid_channel},
    {"send rejects oversized payload", send_too_large},
    {"send queue depth limits", send_queue_depth},
    {"send channels are isolated", send_channel_isolation},
    {"queued message becomes a datagram", datagram_sent_for_queued_message},
    {"window blocks until ack", window_blocks_until_ack},
    {"retransmit after timeout", retransmit_after_timeout},
    {"ack bits release window slots", ack_bits_release_slots},
    {"send quota limits frames per tick", quota_limits_frames_per_tick},
    {"priority orders channels", priority_orders_channels},
    {"unreliable channel skips window", unreliable_channel_skips_window},
    {"receive delivers in order", receive_in_order},
    {"receive delivers duplicate once", receive_duplicate_once},
    {"receive reorders out-of-order datagrams", receive_out_of_order},
    {"receive drops out-of-window seq", receive_out_of_window},
    {"delayed ack releases window", delayed_ack_releases_window},
    {"reorder overflow keeps frame unacked", reorder_overflow_keeps_unacked},
    {"set_reliability requires connection", set_reliability_requires_connection},
    {"set_reliability clears channel state", set_reliability_clears_state},
    {"set_reliability restarts ordering", set_reliability_restarts_ordering},
    {"reconnect resets channels", reconnect_resets_channels},
};
} // namespace

int main()
{
    return test::run(TESTS, std::size(TESTS));
}
