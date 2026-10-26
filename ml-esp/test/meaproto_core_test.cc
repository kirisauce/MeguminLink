// Core 连接状态机的宿主单元测试：假时钟与假空口，不依赖 RTOS。
// 用法见同目录 Makefile：cd ml-esp/test && make meaproto-core

#include "meaproto/core.hpp"
#include "meaproto_test_rig.hpp"

#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

using namespace meaproto;

namespace
{
using namespace mptest;

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

void apply_config_changes_rto()
{
    ChannelRig rig;
    rig.connect();

    Config config = DEFAULT_CONFIG;
    config.rto_initial_ms = 500;
    config.rto_max_ms = 500;
    check(rig.core.apply_config(config));
    check_eq(rig.core.config().rto_initial_ms, 500);

    const u8 payload[3] = {4, 5, 6};
    check(rig.core.send(CH_A, payload) == SendResult::Ok);
    rig.core.step(rig.now);
    const std::size_t after_first = rig.link.frames.size();

    // 旧默认值（100ms）下这时早该重传了；新值要等满 500ms
    rig.now += DEFAULT_CONFIG.rto_initial_ms;
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), after_first);

    rig.now += 500 - DEFAULT_CONFIG.rto_initial_ms;
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), after_first + 1);
    check_eq(rig.core.snapshot().datagrams_retransmitted, 1);
}

void apply_config_rejects_invalid()
{
    ChannelRig rig;
    rig.connect();

    Config config = DEFAULT_CONFIG;
    config.rto_initial_ms = 0; // 非法：退避基数不能是 0
    check(!rig.core.apply_config(config));
    check_eq(rig.core.config().rto_initial_ms, DEFAULT_CONFIG.rto_initial_ms);

    config = DEFAULT_CONFIG;
    config.rto_max_ms = config.rto_initial_ms - 1; // 非法：上限低于初始值
    check(!rig.core.apply_config(config));

    config = DEFAULT_CONFIG;
    config.keepalive_interval_ms = 0;
    check(!rig.core.apply_config(config));

    config = DEFAULT_CONFIG;
    config.send_quota_per_tick = 0;
    check(!rig.core.apply_config(config));

    check_eq(rig.core.config().keepalive_interval_ms, DEFAULT_CONFIG.keepalive_interval_ms);
    check_eq(rig.core.config().send_quota_per_tick, DEFAULT_CONFIG.send_quota_per_tick);
}

void apply_config_ignores_size_fields()
{
    ChannelRig rig;
    rig.connect();

    Config config = DEFAULT_CONFIG;
    config.datagram_window = 1;
    config.max_message_size = 1;
    config.rx_queue_depth = 1;
    config.max_reassembly_slots = 1;
    check(rig.core.apply_config(config));

    check_eq(rig.core.config().datagram_window, DEFAULT_CONFIG.datagram_window);
    check_eq(rig.core.config().max_message_size, DEFAULT_CONFIG.max_message_size);
    check_eq(rig.core.config().rx_queue_depth, DEFAULT_CONFIG.rx_queue_depth);
    check_eq(rig.core.config().max_reassembly_slots, DEFAULT_CONFIG.max_reassembly_slots);
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

void reorder_depth_capped_by_bitmap()
{
    constexpr ChannelSpec wide[] = {{CH_S, Reliability::ReliableOrdered, 32, 8, 1, 64, 4}};
    constexpr ChannelSpec capped[] = {{CH_S, Reliability::ReliableOrdered, 32, 8, 1, 32, 4}};

    check_eq(Core::storage_bytes(wide, DEFAULT_CONFIG, MTU), Core::storage_bytes(capped, DEFAULT_CONFIG, MTU));
}

// ---- 接收侧：去重、排序与延迟确认 ----

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

void unreliable_delivers_out_of_order()
{
    Duo duo;
    duo.connect();

    const u8 first[1] = {1};
    const u8 second[1] = {2};
    check(duo.a.core.send(CH_C, first) == SendResult::Ok);
    check(duo.a.core.send(CH_C, second) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 2);
    const std::vector<u8> f0 = duo.a.link.frames[0];
    const std::vector<u8> f1 = duo.a.link.frames[1];
    duo.a.link.frames.clear();

    duo.b.core.receive(duo.b.now, std::span<const u8>{f1.data(), f1.size()});
    duo.b.core.receive(duo.b.now, std::span<const u8>{f0.data(), f0.size()});

    check_eq(duo.b.sink.messages.size(), 2);
    check(duo.b.sink.messages[0] == std::vector<u8>{2});
    check(duo.b.sink.messages[1] == std::vector<u8>{1});
    check_eq(duo.b.core.snapshot().messages_delivered, 2);
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

// ---- 丢包、重复与序号回绕 ----

/** 持续排满发送队列并推进双方时钟，直到接收端收齐 total 条或耗尽 tick 预算 */
bool deliver_all(Duo &duo, ChannelId channel, int total, u32 limit_ms)
{
    int queued = 0;
    for (u32 elapsed = 0; elapsed < limit_ms; ++elapsed)
    {
        while (queued < total && duo.a.core.can_send(channel))
        {
            const u8 payload[2] = {static_cast<u8>(queued & 0xFF), static_cast<u8>((queued >> 8) & 0xFF)};
            if (duo.a.core.send(channel, payload) != SendResult::Ok)
            {
                return false;
            }
            ++queued;
        }

        duo.step(1);
        duo.flush(duo.a, duo.b);
        duo.flush(duo.b, duo.a);

        if (queued == total && duo.b.sink.messages.size() >= static_cast<std::size_t>(total))
        {
            return true;
        }
    }
    return false;
}

/** 收到的载荷是否正好是 0..total-1 按序 */
bool payloads_in_order(const FakeSink &sink, int total)
{
    if (sink.messages.size() != static_cast<std::size_t>(total))
    {
        return false;
    }
    for (int i = 0; i < total; ++i)
    {
        const std::vector<u8> &message = sink.messages[i];
        if (message.size() != 2 || message[0] != static_cast<u8>(i & 0xFF) ||
            message[1] != static_cast<u8>((i >> 8) & 0xFF))
        {
            return false;
        }
    }
    return true;
}

void duplicate_frames_delivered_once()
{
    Duo duo;
    duo.connect();

    const u8 payload[2] = {3, 3};
    check(duo.a.core.send(CH_A, payload) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    const std::vector<u8> frame = duo.a.link.frames.back();
    duo.a.link.frames.clear();

    for (int i = 0; i < 3; ++i)
    {
        duo.b.core.receive(duo.b.now, std::span<const u8>{frame.data(), frame.size()});
    }

    check_eq(duo.b.sink.messages.size(), 1);
    check_eq(duo.b.core.snapshot().duplicate_datagrams, 2);
}

void lossy_delivery_completes()
{
    Duo duo;
    duo.connect();
    duo.drop_percent = 10;

    check(deliver_all(duo, CH_S, 100, 20000));
    check(payloads_in_order(duo.b.sink, 100));
}

void heavy_loss_still_completes()
{
    Duo duo;
    duo.connect();
    duo.drop_percent = 33;

    check(deliver_all(duo, CH_S, 50, 60000));
    check(payloads_in_order(duo.b.sink, 50));
}

void duplicated_link_still_orders()
{
    Duo duo;
    duo.connect();
    duo.duplicate_percent = 25;

    check(deliver_all(duo, CH_S, 200, 20000));
    check(payloads_in_order(duo.b.sink, 200));
}

void loss_and_duplicates_stay_ordered()
{
    Duo duo;
    duo.connect();
    duo.drop_percent = 2;
    duo.duplicate_percent = 15;

    check(deliver_all(duo, CH_S, 1000, 120000));
    check(payloads_in_order(duo.b.sink, 1000));
}

void sequence_wraparound()
{
    Duo duo;
    duo.connect();

    check(deliver_all(duo, CH_S, 65536, 200000));
    check(payloads_in_order(duo.b.sink, 65536));
    check_eq(duo.b.core.snapshot().duplicate_datagrams, 0);
}

// ---- 保活、超时与 RTT ----

void keepalive_when_idle()
{
    ChannelRig rig;
    rig.connect();
    rig.link.frames.clear();

    rig.now += DEFAULT_CONFIG.keepalive_interval_ms;
    rig.core.step(rig.now);

    check_eq(rig.link.frames.size(), 1);
    check_eq(decoded(rig.link.frames.back()).type, ControlType::KeepAlive);

    // 同一间隔内不重复发
    rig.core.step(rig.now);
    check_eq(rig.link.frames.size(), 1);
}

void silence_times_out()
{
    ChannelRig rig;
    rig.connect();
    rig.link.frames.clear();

    rig.now += DEFAULT_CONFIG.connect_timeout_ms + 1;
    rig.core.step(rig.now);

    check(!rig.core.connected());
    check_eq(rig.sink.disconnected_count, 1);
    check(rig.sink.last_reason == DisconnectReason::Timeout);
}

void keepalive_keeps_both_alive()
{
    Duo duo;
    duo.connect();

    for (u32 elapsed = 0; elapsed < 20000; elapsed += 250)
    {
        duo.step(250);
        duo.flush(duo.a, duo.b);
        duo.flush(duo.b, duo.a);
    }

    check(duo.a.core.connected());
    check(duo.b.core.connected());
    check_eq(duo.a.sink.disconnected_count, 0);
}

void ping_samples_rtt()
{
    Duo duo;
    duo.connect();

    check(duo.a.core.ping(duo.a.now));
    duo.flush(duo.a, duo.b);
    duo.a.now += 40;
    duo.b.now += 40;
    duo.flush(duo.b, duo.a);

    check(duo.a.core.snapshot().rtt_ms > 0);
    check(duo.a.core.snapshot().rto_ms >= DEFAULT_CONFIG.rto_initial_ms);
}

void pong_with_wrong_token_ignored()
{
    Duo duo;
    duo.connect();

    check(duo.a.core.ping(duo.a.now));
    duo.flush(duo.a, duo.b);
    duo.a.link.frames.clear();

    ControlFrame pong;
    pong.type = ControlType::Pong;
    pong.token = 999;
    pong.own_epoch = duo.a.core.peer_epoch();
    const std::vector<u8> bytes = encode_control(pong);
    duo.a.core.receive(duo.a.now, std::span<const u8>{bytes.data(), bytes.size()});

    check_eq(duo.a.core.snapshot().rtt_ms, 0);
}

void ping_requires_connection()
{
    ChannelRig rig;
    check(!rig.core.ping(rig.now));
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
    {"apply_config changes rto", apply_config_changes_rto},
    {"apply_config rejects invalid", apply_config_rejects_invalid},
    {"apply_config ignores size fields", apply_config_ignores_size_fields},
    {"send quota limits frames per tick", quota_limits_frames_per_tick},
    {"priority orders channels", priority_orders_channels},
    {"unreliable channel skips window", unreliable_channel_skips_window},
    {"reorder depth capped by ack bitmap", reorder_depth_capped_by_bitmap},
    {"receive delivers in order", receive_in_order},
    {"receive delivers duplicate once", receive_duplicate_once},
    {"receive reorders out-of-order datagrams", receive_out_of_order},
    {"unreliable channel delivers out of order", unreliable_delivers_out_of_order},
    {"receive drops out-of-window seq", receive_out_of_window},
    {"delayed ack releases window", delayed_ack_releases_window},
    {"reorder overflow keeps frame unacked", reorder_overflow_keeps_unacked},
    {"set_reliability requires connection", set_reliability_requires_connection},
    {"set_reliability clears channel state", set_reliability_clears_state},
    {"set_reliability restarts ordering", set_reliability_restarts_ordering},
    {"reconnect resets channels", reconnect_resets_channels},
    {"duplicate frames delivered once", duplicate_frames_delivered_once},
    {"keepalive sent when idle", keepalive_when_idle},
    {"silence times out connection", silence_times_out},
    {"keepalive keeps both alive", keepalive_keeps_both_alive},
    {"ping samples rtt", ping_samples_rtt},
    {"pong with wrong token ignored", pong_with_wrong_token_ignored},
    {"ping requires connection", ping_requires_connection},
    {"lossy link still delivers", lossy_delivery_completes},
    {"heavy loss still delivers", heavy_loss_still_completes},
    {"duplicated link still orders", duplicated_link_still_orders},
    {"loss and duplicates stay in order", loss_and_duplicates_stay_ordered},
    {"sequence wraps without loss", sequence_wraparound},
};
} // namespace

int main()
{
    return test::run(TESTS, std::size(TESTS));
}
