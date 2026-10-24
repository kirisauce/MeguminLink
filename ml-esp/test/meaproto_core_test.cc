// Core 连接状态机的宿主单元测试：假时钟与假空口，不依赖 RTOS。
// 用法见同目录 Makefile：cd ml-esp/test && make meaproto-core

#include "meaproto/core.hpp"
#include "meaproto/hash.hpp"
#include "meaproto/wire.hpp"

#include <cstddef>
#include <print>
#include <source_location>
#include <span>
#include <vector>

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

constexpr u16 MTU = 1470;
constexpr u32 START_MS = 1000;
constexpr u32 FIRST_EPOCH = 0x01020304;

struct FakeSink final : Sink
{
    void on_message(ChannelId, std::span<const u8>) override
    {
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
        : core(std::span<const ChannelSpec>{}, Config{}, MTU, sink,
               CoreHooks{&link, &FakeLink::send, &FakeLink::random})
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

void idle_initial()
{
    Rig rig;
    check(rig.core.state() == Core::State::Idle);
    check(!rig.core.connected());
    check(!rig.core.connecting());
    check(rig.link.frames.empty());
    check_eq(rig.core.step(rig.now), rig.now + 1000);
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
    check_eq(frame.config_hash, config_hash(std::span<const ChannelSpec>{}));
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
    const ControlFrame frame = decoded(rig.link.frames.back());
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
    Stats idle = rig.core.snapshot();
    check(!idle.connected);
    check_eq(idle.peer_epoch, 0);

    rig.core.open(rig.now);
    Stats connecting = rig.core.snapshot();
    check(!connecting.connected);
    check_eq(connecting.peer_epoch, 0);
}

struct TestCase
{
    const char *name;
    void (*run)();
};

const TestCase TESTS[] = {
    {"初始状态", idle_initial},
    {"发起握手", open_sends_connect_request},
    {"重复 open 幂等", open_idempotent},
    {"close 发 Disconnect", close_sends_disconnect},
    {"Idle 时 close 无操作", close_when_idle_is_noop},
    {"发送失败仍进 Connecting", send_failure_keeps_connecting},
    {"快照反映状态", snapshot_reflects_state},
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
