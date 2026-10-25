// 分片与重组的宿主单元测试：假时钟与假空口，不依赖 RTOS。
// 用法见同目录 Makefile：cd ml-esp/test && make meaproto-fragment

#include "meaproto/core.hpp"
#include "meaproto_test_rig.hpp"

#include <cstddef>
#include <span>
#include <vector>

using namespace meaproto;

namespace
{
using namespace mptest;

constexpr ChannelId CH_BIG{7};
constexpr ChannelId CH_RAW{8};

constexpr usize BIG_MESSAGE = 8192;

// clang-format off
/** 通道表：BIG 可靠 8KB 上限、RAW 不可靠 8KB 上限 */
inline constexpr ChannelSpec FRAGMENT_CHANNELS[] = {
    {CH_BIG, Reliability::ReliableOrdered, BIG_MESSAGE, 16, 1, 4, 8},
    {CH_RAW, Reliability::Unreliable, BIG_MESSAGE, 2, 2, 2, 2},
};
// clang-format on

/** 造一段有区分度的载荷 */
std::vector<u8> pattern(usize size, u8 seed = 0)
{
    std::vector<u8> data(size);
    for (usize i = 0; i < size; ++i)
    {
        data[i] = static_cast<u8>((i * 31 + seed * 7) & 0xFF);
    }
    return data;
}

/** 一直推进到接收端收到 want 条消息 */
bool pump_until(Duo &duo, usize want, u32 limit_ms)
{
    for (u32 elapsed = 0; elapsed < limit_ms; ++elapsed)
    {
        duo.step(1);
        duo.flush(duo.a, duo.b);
        duo.flush(duo.b, duo.a);
        if (duo.b.sink.messages.size() >= want)
        {
            return true;
        }
    }
    return false;
}

void whole_message_reassembles()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();

    const std::vector<u8> data = pattern(BIG_MESSAGE);
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    check(pump_until(duo, 1, 2000));

    check_eq(duo.b.sink.messages.size(), 1);
    check(duo.b.sink.messages[0] == data);
    check_eq(duo.b.core.snapshot().fragments_reassembled, 1);
    check_eq(duo.b.core.snapshot().messages_delivered, 1);
}

void fragmented_message_survives_loss()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();
    duo.drop_percent = 15;

    const std::vector<u8> data = pattern(BIG_MESSAGE, 1);
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    check(pump_until(duo, 1, 20000));

    check(duo.b.sink.messages[0] == data);
    check_eq(duo.b.core.snapshot().fragments_reassembled, 1);
}

void slot_shortage_recovers()
{
    Duo duo(FRAGMENT_CHANNELS, Config{.max_reassembly_slots = 1});
    duo.connect();

    const std::vector<u8> first = pattern(2000, 1);
    const std::vector<u8> second = pattern(2000, 2);
    check(duo.a.core.send(CH_BIG, first) == SendResult::Ok);
    check(duo.a.core.send(CH_BIG, second) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 4);
    const std::vector<std::vector<u8>> frames = duo.a.link.frames;
    duo.a.link.frames.clear();

    // 交错投递：第二条的首片在第一条未收齐时到达，唯一槽被占满而拒收
    const u8 order[4] = {0, 2, 1, 3};
    for (const u8 index : order)
    {
        duo.b.core.receive(duo.b.now, std::span<const u8>{frames[index].data(), frames[index].size()});
    }

    check_eq(duo.b.sink.messages.size(), 1);
    check(duo.b.core.snapshot().messages_dropped >= 1);

    // 被拒的片收不到确认，靠重传补齐
    check(pump_until(duo, 2, 20000));
    check(duo.b.sink.messages[0] == first);
    check(duo.b.sink.messages[1] == second);
}

void incomplete_message_waits_for_retransmission()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();

    const std::vector<u8> data = pattern(3000, 3);
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 3);
    const std::vector<std::vector<u8>> frames = duo.a.link.frames;
    duo.a.link.frames.clear();

    // 首片与末片先到，中间那片缺失：消息压住不交付，槽保持占用
    duo.b.core.receive(duo.b.now, std::span<const u8>{frames[0].data(), frames[0].size()});
    duo.b.core.receive(duo.b.now, std::span<const u8>{frames[2].data(), frames[2].size()});
    check_eq(duo.b.sink.messages.size(), 0);

    // 缺片靠重传补齐，期间连接不掉线
    check(pump_until(duo, 1, 20000));
    check(duo.b.sink.messages[0] == data);
    check_eq(duo.b.core.snapshot().fragments_reassembled, 1);
}

void disconnect_clears_reassembly()
{
    Duo duo(FRAGMENT_CHANNELS, Config{.max_reassembly_slots = 1});
    duo.connect();

    const std::vector<u8> data = pattern(3000, 4);
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 3);
    const std::vector<u8> head = duo.a.link.frames.front();
    duo.a.link.frames.clear();
    duo.b.core.receive(duo.b.now, std::span<const u8>{head.data(), head.size()});

    duo.a.core.close(duo.a.now);
    duo.flush(duo.a, duo.b);
    check(!duo.a.core.connected());
    check(!duo.b.core.connected());

    duo.a.core.open(duo.a.now);
    duo.flush(duo.a, duo.b);
    duo.flush(duo.b, duo.a);
    check(duo.a.core.connected());
    check(duo.b.core.connected());

    // 重连后 split_id 从 1 重新开始：旧槽若残留会把内容搅乱
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    check(pump_until(duo, 1, 20000));
    check(duo.b.sink.messages[0] == data);
}

void reliability_switch_clears_reassembly()
{
    Duo duo(FRAGMENT_CHANNELS, Config{.max_reassembly_slots = 1});
    duo.connect();

    const std::vector<u8> data = pattern(3000, 5);
    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    duo.a.core.step(duo.a.now);

    check_eq(duo.a.link.frames.size(), 3);
    const std::vector<u8> head = duo.a.link.frames.front();
    duo.a.link.frames.clear();
    duo.b.core.receive(duo.b.now, std::span<const u8>{head.data(), head.size()});

    // 两端同时切换可靠性：序号与未收齐的分片一起作废
    check(duo.a.core.set_reliability(CH_BIG, Reliability::Unreliable));
    check(duo.b.core.set_reliability(CH_BIG, Reliability::Unreliable));
    check(duo.a.core.set_reliability(CH_BIG, Reliability::ReliableOrdered));
    check(duo.b.core.set_reliability(CH_BIG, Reliability::ReliableOrdered));

    check(duo.a.core.send(CH_BIG, data) == SendResult::Ok);
    check(pump_until(duo, 1, 20000));
    check(duo.b.sink.messages[0] == data);
    check_eq(duo.b.core.snapshot().fragments_reassembled, 1);
}

void unreliable_message_rejected_when_oversized()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();

    const std::vector<u8> oversized = pattern(2000);
    check(duo.a.core.send(CH_RAW, oversized) == SendResult::TooLarge);

    const std::vector<u8> fits = pattern(100);
    check(duo.a.core.send(CH_RAW, fits) == SendResult::Ok);
    check(pump_until(duo, 1, 1000));
    check(duo.b.sink.messages[0] == fits);
}

void send_reserves_whole_message()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();

    const std::vector<u8> big = pattern(BIG_MESSAGE, 5);
    check(duo.a.core.send(CH_BIG, big) == SendResult::Ok); // 6 片
    check(duo.a.core.send(CH_BIG, big) == SendResult::Ok); // 共 12 片
    check(duo.a.core.send(CH_BIG, big) == SendResult::Busy);

    check(pump_until(duo, 2, 20000));
    check(duo.a.core.send(CH_BIG, big) == SendResult::Ok);
}

void fragments_keep_ordering_with_small_messages()
{
    Duo duo(FRAGMENT_CHANNELS);
    duo.connect();

    const std::vector<u8> big = pattern(2000, 6);
    const std::vector<u8> small = {1, 2, 3};
    check(duo.a.core.send(CH_BIG, big) == SendResult::Ok);
    check(duo.a.core.send(CH_BIG, small) == SendResult::Ok);

    check(pump_until(duo, 2, 20000));
    check(duo.b.sink.messages[0] == big);
    check(duo.b.sink.messages[1] == small);
}

const test::Case TESTS[] = {
    {"8KB message reassembles", whole_message_reassembles},
    {"fragments survive loss", fragmented_message_survives_loss},
    {"slot shortage recovers", slot_shortage_recovers},
    {"incomplete message waits for retransmission", incomplete_message_waits_for_retransmission},
    {"disconnect clears reassembly", disconnect_clears_reassembly},
    {"reliability switch clears reassembly", reliability_switch_clears_reassembly},
    {"unreliable oversized rejected", unreliable_message_rejected_when_oversized},
    {"send reserves whole message", send_reserves_whole_message},
    {"fragments keep ordering", fragments_keep_ordering_with_small_messages},
};
} // namespace

int main()
{
    return test::run(TESTS, std::size(TESTS));
}
