// ARQ 控制算法（meguminlink/wireless/arq.hpp）的宿主机单元测试：把算法核心放在假时钟与
// 假空口上跑，覆盖丢帧、MAC 失败、重复、乱序、序号回绕、熔断与流控，不需要烧板。
// 用法见同目录 Makefile：cd ml-esp/test && make run
//
// 仿真模型：A 端投递消息、B 端只回确认，两端各持一份 Sender + Receiver；帧在空口
// 上按固定时延投递，且携带发送端 receiver 的累积确认号与 SACK 位图——确认与数据
// 一样会丢，这正是"命令到了但确认丢了"的那条路径。
//
// 这条单向模型覆盖了收发两个方向共用的全部算法；重复帧触发"重放缓存响应"那一步
// 属于宿主侧（命令序号到响应序号的映射在链路适配层），这里只直接测 Sender::resend。

#include "meguminlink/wireless/arq.hpp"

#include <cstring>
#include <deque>
#include <print>
#include <vector>

using namespace mlink;
using namespace mlink::arq;

namespace
{
constexpr u16 SLOT = 8;   /**< 载荷只放一个 u32 的应用 id */
constexpr u32 SEEDS = 24; /**< 随机类用例的种子数：单次跑通说明不了问题 */

int g_pass = 0;
int g_fail = 0;

// clang-format off
#define CHECK(cond) do { if (cond) { ++g_pass; } else { ++g_fail; std::print("    [FAIL] {}:{}  {}\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ == vb_) { ++g_pass; } else { ++g_fail; std::print("    [FAIL] {}:{}  {} == {}  (got {}, want {})\n", __FILE__, __LINE__, #a, #b, va_, vb_); } } while (0)
// clang-format on

/** 空口参数 */
struct Link
{
    u32 seed = 0x12345678u;
    u32 latency_ms = 1;
    int data_loss = 0;       /**< 静默丢数据帧（千分比，不回报 MAC 失败） */
    int ack_loss = 0;        /**< 丢纯 ACK 帧 */
    int mac_fail = 0;        /**< 数据帧丢失并回报 MAC 发送失败 */
    bool blackout = false;   /**< 全丢 */
    int duplicate_every = 0; /**< 每 N 帧复制一份 */
    u32 reorder_hold_ms = 0; /**< 周期性延迟数据帧，制造乱序 */
    u32 drop_first_data = 0; /**< 会话起始静默丢弃前 N 个数据帧 */
};

struct Frame
{
    u32 at = 0;
    int src = 0;
    bool ack_only = false;
    bool gap_before = false;
    u16 seq = 0;
    u16 ack = 0;
    u16 sack = 0;
    u8 win = 0;
    std::vector<u8> payload;
};

/** 一个端点：算法核心 + 载荷存储 + 宿主侧记账 */
struct Endpoint
{
    Sender tx;
    Receiver rx;
    std::vector<u8> tx_store; /**< 发送窗口存储（一次性按 MAX_WINDOW 分配） */
    std::vector<u8> rx_store; /**< 接收载荷存储（重排缓冲，按 MAX_WINDOW 分槽） */
    std::vector<u16> rx_len;
    u32 window_full = 0;  /**< submit 因窗口满被挡下的次数 */
    u32 breaker_open = 0; /**< submit 因熔断被快速失败的次数 */
    u32 replay = 0;       /**< resend 命中（重放成功） */
    u32 replay_miss = 0;  /**< resend 未命中（序号已不在窗口内） */
    bool ack_scheduled = false;
    u32 ack_due = 0;
};

/** 一次场景跑完后的关键结果，供用例断言 */
struct Outcome
{
    bool finished = false;
    bool ordered = false; /**< 交付序列恰为 0..messages-1 */
    bool broken = false;
    u32 got = 0;
    u32 retx = 0;
    u32 failed = 0;
    u32 trip = 0;
    u32 buffered = 0;
    u32 oow = 0;
    u32 dup = 0;
    u32 skipped = 0;
    u32 window_full = 0;
    u32 breaker_open = 0;
};

struct Sim;

struct EpContext
{
    Sim *sim = nullptr;
    int idx = 0;
};

/** 仿真世界：ep[0] 投递消息，ep[1] 只回确认 */
struct Sim
{
    Config cfg;
    Link link;
    Endpoint ep[2];
    EpContext ctx[2];
    std::vector<Frame> wire;
    std::deque<u32> outbox; /**< A 端待投递的应用 id */
    std::vector<u32> got;   /**< B 端收到的 id 顺序 */
    u32 now = 0;
    u32 rng = 1;
    u32 frames_sent = 0;
    u32 data_sent = 0;

    void init(const Config &config, const Link &link_config)
    {
        cfg = config;
        link = link_config;
        now = 0;
        rng = (link.seed != 0) ? link.seed : 1u;
        frames_sent = 0;
        data_sent = 0;
        wire.clear();
        outbox.clear();
        got.clear();

        for (int i = 0; i < 2; ++i)
        {
            Endpoint &e = ep[i];
            e = Endpoint{};
            e.tx_store.assign(static_cast<size_t>(MAX_WINDOW) * SLOT, 0);
            e.rx_store.assign(static_cast<size_t>(MAX_WINDOW) * SLOT, 0);
            e.rx_len.assign(MAX_WINDOW, 0);
            e.tx.init(cfg, e.tx_store.data(), SLOT, MAX_WINDOW);
            e.rx.init(cfg);
            ctx[i] = EpContext{this, i};
            e.tx.set_callbacks(&ctx[i], &Sim::on_transmit, nullptr);
        }
    }

    u32 next_random()
    {
        u32 x = rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return rng = x;
    }

    static void on_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before)
    {
        (void)retransmit;
        auto *c = static_cast<EpContext *>(raw);
        c->sim->send_frame(c->idx, false, seq, payload, len, true, gap_before);
    }

    /** 按应用层载荷大小提交一条消息；窗口满或熔断时记数并返回 false */
    bool try_submit(u32 id)
    {
        u8 buf[4];
        std::memcpy(buf, &id, sizeof(id));
        u16 seq = 0;
        const SubmitResult r = ep[0].tx.submit(buf, sizeof(buf), now, &seq);
        if (r == SubmitResult::WindowFull)
        {
            ep[0].window_full++;
        }
        else if (r == SubmitResult::BreakerOpen)
        {
            ep[0].breaker_open++;
        }
        return r == SubmitResult::Ok;
    }

    void send_frame(int src, bool ack_only, u16 seq, const u8 *payload, u16 len, bool reliable, bool gap_before)
    {
        Endpoint &e = ep[src];

        Frame frame;
        frame.src = src;
        frame.ack_only = ack_only;
        frame.gap_before = gap_before;
        frame.seq = seq;
        frame.ack = e.rx.ack();
        frame.sack = e.rx.sack_bitmap();
        frame.win = e.rx.window();
        frame.at = now + link.latency_ms;

        if (!ack_only)
        {
            frame.payload.assign(payload, payload + len);
            e.rx.clear_ack_pending(); // 数据帧捎带确认即完成确认义务
            e.ack_scheduled = false;
        }

        // 乱序制造：周期性延迟一部分数据帧，让后续帧超车
        if (link.reorder_hold_ms > 0 && !ack_only && (next_random() % 3u) == 0)
        {
            frame.at += link.reorder_hold_ms;
        }

        frames_sent++;

        const int loss = ack_only ? link.ack_loss : link.data_loss;
        const bool mac_fail = !ack_only && reliable && (static_cast<int>(next_random() % 1000u) < link.mac_fail);
        bool dropped = link.blackout || mac_fail || (static_cast<int>(next_random() % 1000u) < loss);

        // 会话起始丢前 N 个数据帧：第一个到达的帧因此不是序号初值
        if (!ack_only && reliable && ++data_sent <= link.drop_first_data)
        {
            dropped = true;
        }

        if (dropped)
        {
            if (mac_fail)
            {
                // MAC 层发送失败回执。真实链路上"确认丢了但帧其实到了"是常态，
                // 算法只能把它当成提前重传的提示
                e.tx.on_mac_failure(now);
            }
            return;
        }

        wire.push_back(frame);
        if (link.duplicate_every > 0 && !ack_only && (frames_sent % static_cast<u32>(link.duplicate_every)) == 0)
        {
            Frame dup = frame;
            dup.at += 1;
            wire.push_back(dup);
        }
    }

    void deliver(Frame &frame)
    {
        Endpoint &dst = ep[1 - frame.src];

        // 任何到达的帧都携带对端接收状态的确认
        dst.tx.on_ack(frame.ack, frame.sack, frame.win, now);
        if (frame.ack_only)
        {
            return;
        }
        if (frame.src != 0)
        {
            return; // 本模型只让 A 端投递数据（见文件头说明）
        }

        const RxResult result = dst.rx.on_frame(frame.seq, frame.gap_before, now);

        if (result == RxResult::Duplicate || result == RxResult::OutOfWindow)
        {
            return; // 重复帧不回交付、超窗帧等对端重传；确认由核心置的 ack_pending 负责
        }

        const size_t slot = static_cast<size_t>(frame.seq % MAX_WINDOW);
        std::memcpy(dst.rx_store.data() + slot * SLOT, frame.payload.data(), frame.payload.size());
        dst.rx_len[slot] = static_cast<u16>(frame.payload.size());

        if (result != RxResult::Delivered)
        {
            return; // 乱序帧已进 SACK 位图，等游标补齐时再交付
        }

        const u16 last = dst.rx.commit(frame.seq, now);
        for (u16 seq = frame.seq;; ++seq)
        {
            u32 id = 0;
            std::memcpy(&id, dst.rx_store.data() + static_cast<size_t>(seq % MAX_WINDOW) * SLOT, sizeof(id));
            got.push_back(id);
            if (seq == last)
            {
                break;
            }
        }
    }

    /** 没有出向帧可捎带确认时，延迟 delayed_ack_ms 补一个纯 ACK */
    void schedule_ack(int idx)
    {
        Endpoint &e = ep[idx];
        if (!e.rx.ack_pending())
        {
            e.ack_scheduled = false;
            return;
        }
        if (!e.ack_scheduled)
        {
            e.ack_scheduled = true;
            e.ack_due = now + cfg.delayed_ack_ms;
            return;
        }
        if (static_cast<i32>(now - e.ack_due) >= 0)
        {
            send_frame(idx, true, 0, nullptr, 0, false, false);
            e.rx.clear_ack_pending();
            e.ack_scheduled = false;
        }
    }

    void step()
    {
        for (size_t i = 0; i < wire.size();)
        {
            if (static_cast<i32>(now - wire[i].at) < 0)
            {
                ++i;
                continue;
            }
            Frame frame = wire[i];
            wire.erase(wire.begin() + static_cast<long>(i));
            deliver(frame);
        }

        schedule_ack(1);
        ep[0].tx.poll(now);
        ep[1].tx.poll(now);

        while (!outbox.empty() && try_submit(outbox.front()))
        {
            outbox.pop_front();
        }
    }

    bool done() const
    {
        return outbox.empty() && ep[0].tx.idle() && ep[1].tx.idle() && wire.empty();
    }

    bool run_until_done(u32 max_ms)
    {
        for (u32 i = 0; i < max_ms; ++i)
        {
            step();
            if (done())
            {
                return true;
            }
            now++;
        }
        return false;
    }

    Outcome outcome(u32 messages) const
    {
        Outcome o;
        o.finished = done();
        o.got = static_cast<u32>(got.size());
        o.broken = ep[0].tx.broken();

        const SenderSnapshot tx = ep[0].tx.snapshot();
        const ReceiverSnapshot rx = ep[1].rx.snapshot();
        o.retx = tx.retx_frames;
        o.failed = tx.failed_frames;
        o.trip = tx.breaker_trips;
        o.buffered = rx.buffered;
        o.oow = rx.out_of_window;
        o.dup = rx.duplicate;
        o.skipped = rx.skipped;
        o.window_full = ep[0].window_full;
        o.breaker_open = ep[0].breaker_open;

        o.ordered = (got.size() == messages);
        for (u32 i = 0; o.ordered && i < messages; ++i)
        {
            o.ordered = (got[i] == i);
        }
        return o;
    }
};

/** 跑一个完整场景：messages 条消息全部投递，直到链路静默或超时 */
Outcome run_case(const Config &cfg, const Link &link, u32 messages, u32 max_ms)
{
    Sim sim;
    sim.init(cfg, link);
    for (u32 i = 0; i < messages; ++i)
    {
        sim.outbox.push_back(i);
    }
    sim.run_until_done(max_ms);
    return sim.outcome(messages);
}

/**
 * 多种子扫描：每个种子都必须做到"消息不丢、交付严格按序、没有消息被判失败"，
 * 返回各计数的总和供用例追加"该场景真的触发了某条路径"的断言。
 *
 * 随机类场景单跑一个种子说明不了问题：第一版的核心在单种子下全绿，扫到第二十几个
 * 种子才暴露出会话起始丢帧时序号基准猜错的缺陷。
 */
template <typename Fn> Outcome sweep(const char *what, Fn run_one)
{
    Outcome sum;
    u32 ok = 0;
    for (u32 seed = 1; seed <= SEEDS; ++seed)
    {
        const Outcome o = run_one(seed);
        if (o.finished && o.ordered && o.failed == 0)
        {
            ok++;
        }
        else
        {
            std::print("    [FAIL] {} seed={}: 完成={} 交付={} 重传={} 失败={} 缓冲={} 超窗={}\n", what, seed,
                       o.finished, o.got, o.retx, o.failed, o.buffered, o.oow);
        }
        sum.retx += o.retx;
        sum.failed += o.failed;
        sum.trip += o.trip;
        sum.buffered += o.buffered;
        sum.oow += o.oow;
        sum.dup += o.dup;
        sum.skipped += o.skipped;
    }
    CHECK_EQ(ok, SEEDS);
    return sum;
}

// ---- 用例 ----

void test_helpers()
{
    // 序号模比较在回绕点的行为：裸 < 比较会在这里整窗翻车
    CHECK(seq_before(65534u, 65535u));
    CHECK(!seq_before(65535u, 65535u));
    CHECK(seq_before(0u, 1u));
    CHECK(!seq_before(1u, 0u));
    CHECK(seq_before(65535u, 0u));
    CHECK(!seq_before(0u, 65535u));
    CHECK_EQ(seq_diff(0u, 65535u), 1u);

    // SACK 编码：位 i 表示 seq == ack + 1 + i，累积确认号本身不在位图里
    u16 bitmap = sack_set(sack_set(0, 100, 101), 100, 103);
    CHECK(sack_test(bitmap, 100, 101));
    CHECK(!sack_test(bitmap, 100, 102));
    CHECK(sack_test(bitmap, 100, 103));
    CHECK(!sack_test(bitmap, 100, 100));
    CHECK(!sack_test(bitmap, 100, 116));
}

void test_clean_run()
{
    Config cfg;
    const Outcome o = run_case(cfg, Link{}, 30, 20000);
    CHECK(o.finished);
    CHECK(o.ordered);
    CHECK_EQ(o.retx, 0u);
    CHECK_EQ(o.failed, 0u);
    CHECK_EQ(o.dup, 0u);
}

void test_lossy_link()
{
    const Outcome s = sweep("丢帧链路", [](u32 seed) {
        Link link;
        link.seed = seed;
        link.data_loss = 100; // 10% 丢数据帧
        link.ack_loss = 50;   // 5% 丢纯 ACK
        return run_case(Config{}, link, 60, 60000);
    });
    CHECK(s.retx > 0); // 必须真的丢过并被重传吸收，否则这个用例是白跑
    CHECK_EQ(s.trip, 0u);
}

void test_mac_failure_fast_retransmit()
{
    const Outcome s = sweep("MAC 发送失败", [](u32 seed) {
        Link link;
        link.seed = seed;
        link.mac_fail = 200; // 20% 的帧丢失并回报 MAC 失败
        return run_case(Config{}, link, 40, 60000);
    });
    CHECK(s.retx > 0);
}

void test_duplicate_frames_never_repeat_delivery()
{
    const Outcome s = sweep("重复帧", [](u32 seed) {
        Link link;
        link.seed = seed;
        link.duplicate_every = 3;
        return run_case(Config{}, link, 40, 60000);
    });
    CHECK(s.dup > 0); // 注入确实生效，去重路径被走到
}

void test_reorder_with_sack()
{
    const Outcome s = sweep("乱序 + SACK", [](u32 seed) {
        Config cfg;
        cfg.window = 8; // 窗口 > 1 才可能乱序
        cfg.reorder = true;
        Link link;
        link.seed = seed;
        link.reorder_hold_ms = 6;
        return run_case(cfg, link, 40, 60000);
    });
    CHECK(s.buffered > 0); // 乱序确实发生过并记入 SACK
}

void test_reorder_disabled_falls_back_to_retransmit()
{
    const Outcome s = sweep("不支持重排", [](u32 seed) {
        Config cfg;
        cfg.window = 8;
        cfg.reorder = false;
        Link link;
        link.seed = seed;
        link.reorder_hold_ms = 6;
        return run_case(cfg, link, 40, 60000);
    });
    CHECK_EQ(s.buffered, 0u); // 乱序帧一律被拒，不进位图
    CHECK(s.oow > 0);
    CHECK(s.retx > 0); // 靠重传补齐
}

void test_session_start_with_lost_frames()
{
    // 会话起始丢前几帧：第一个到达的帧不再是序号初值。接收端若拿第一个到达的帧
    // 定基准，就会把还没收到的帧判成重复帧丢弃，同时用累积确认号把它们"确认"掉，
    // 消息静默消失——这正是基准必须由调用方显式给出的原因。
    sweep("会话起始丢帧", [](u32 seed) {
        Config cfg;
        cfg.window = 8;
        cfg.reorder = true;
        Link link;
        link.seed = seed;
        link.drop_first_data = 3;
        link.reorder_hold_ms = 4;
        return run_case(cfg, link, 40, 120000);
    });
}

void test_window_pipelining()
{
    Config cfg;
    cfg.window = 8;
    cfg.reorder = true;
    Link link;
    link.latency_ms = 2;

    Sim sim;
    sim.init(cfg, link);
    for (u32 i = 0; i < 200; ++i)
    {
        sim.outbox.push_back(i);
    }
    CHECK(sim.run_until_done(60000));

    const Outcome o = sim.outcome(200);
    CHECK(o.ordered);
    CHECK(sim.ep[0].tx.snapshot().in_flight <= 8);
}

void test_sequence_wraparound()
{
    // 跑过 u16 回绕点：序号比较若用裸 < 会在 65535→0 处把整窗判成过期
    Config cfg;
    cfg.window = 8;
    cfg.reorder = true;

    Sim sim;
    sim.init(cfg, Link{});

    const u32 kN = 70000;
    u32 submitted = 0;
    for (u32 ms = 0; ms < 2000000; ++ms)
    {
        while (submitted < kN && sim.outbox.empty())
        {
            sim.outbox.push_back(submitted++);
        }
        sim.step();
        sim.now++;
        if (submitted == kN && sim.done())
        {
            break;
        }
    }

    const Outcome o = sim.outcome(kN);
    CHECK(o.finished);
    CHECK(o.ordered);
    CHECK_EQ(o.retx, 0u);
    CHECK_EQ(o.failed, 0u);
}

void test_failure_budget()
{
    Link link;
    link.blackout = true;

    Sim sim;
    sim.init(Config{}, link);
    sim.outbox.push_back(0);

    u32 failed_at = 0;
    for (u32 ms = 0; ms < 5000 && sim.ep[0].tx.snapshot().failed_frames == 0; ++ms)
    {
        sim.step();
        sim.now++;
        failed_at = sim.now;
    }

    // 静默全丢：还没有 RTT 样本，RTO 从初始值 60ms 起指数退避
    // （60+120+240+250+250+250），首传 + 6 次重传之后判死
    CHECK_EQ(sim.ep[0].tx.snapshot().failed_frames, 1u);
    CHECK(failed_at >= 1400 && failed_at <= 1450);
    CHECK(failed_at < 1800); // 必须留在主机侧的命令超时之内
    CHECK(sim.ep[0].tx.idle());
}

void test_breaker_and_recovery()
{
    Config cfg;
    Link link;
    link.blackout = true;

    Sim sim;
    sim.init(cfg, link);

    for (int i = 0; i < cfg.breaker_failures; ++i)
    {
        sim.outbox.push_back(static_cast<u32>(i));
        CHECK(sim.run_until_done(5000));
    }

    CHECK_EQ(sim.ep[0].tx.snapshot().failed_frames, static_cast<u32>(cfg.breaker_failures));
    CHECK(sim.ep[0].tx.broken()); // 连续失败达到阈值 → 熔断
    CHECK_EQ(sim.ep[0].tx.snapshot().breaker_trips, 1u);

    // 熔断期间提交快速失败，不再等满重传预算
    sim.now += 100;
    CHECK(!sim.try_submit(100));
    CHECK(sim.ep[0].breaker_open > 0);
    CHECK(sim.ep[0].tx.idle());

    // 恢复链路并等到试探时刻：放行一条并成功 → 熔断复位
    sim.link.blackout = false;
    sim.now += cfg.breaker_probe_ms + 1;
    CHECK(sim.try_submit(200));
    CHECK(sim.run_until_done(20000));
    CHECK(!sim.ep[0].tx.broken());
    CHECK_EQ(sim.got.size(), 1u);
    CHECK_EQ(sim.got[0], 200u);
}

void test_gap_skip_after_give_up()
{
    // 一条消息重传耗尽被放弃后必须在流里留一个可跳过的空洞：否则接收端游标永远
    // 卡在空洞上，之后每一帧都被判成超窗，链路整体僵死。
    Link link;
    link.blackout = true;

    Sim sim;
    sim.init(Config{}, link); // window = 1
    sim.outbox.push_back(100);
    CHECK(sim.run_until_done(5000));
    CHECK_EQ(sim.ep[0].tx.snapshot().failed_frames, 1u);

    // 恢复链路后继续投递：被放弃的那条不该出现，其余必须按序到达
    sim.link.blackout = false;
    for (u32 i = 1; i < 20; ++i)
    {
        sim.outbox.push_back(100 + i);
    }
    CHECK(sim.run_until_done(60000));

    CHECK_EQ(sim.got.size(), 19u);
    for (size_t i = 0; i < sim.got.size(); ++i)
    {
        CHECK_EQ(sim.got[i], 101u + static_cast<u32>(i));
    }
    CHECK(sim.ep[1].rx.snapshot().skipped >= 1);
    CHECK_EQ(sim.ep[0].tx.snapshot().failed_frames, 1u); // 后续消息不该再被判失败
}

void test_flow_control_blocks_on_zero_window()
{
    Sim sim;
    sim.init(Config{}, Link{});

    // 对端通告接收窗口 0：提交必须被窗口满挡住
    sim.ep[0].tx.on_ack(0, 0, 0, sim.now);
    CHECK_EQ(sim.ep[0].tx.window(), 0u);
    CHECK(!sim.try_submit(0));
    CHECK(sim.ep[0].window_full > 0);

    // 窗口重新打开即恢复
    sim.ep[0].tx.on_ack(0, 0, 1, sim.now);
    CHECK_EQ(sim.ep[0].tx.window(), 1u);
    CHECK(sim.try_submit(7));
    CHECK(sim.run_until_done(20000));
    CHECK_EQ(sim.got.size(), 1u);
    CHECK_EQ(sim.got[0], 7u);
}

void test_replay_and_miss()
{
    Sim sim;
    sim.init(Config{}, Link{});

    // 重放一个从未在窗内的序号：必须明确返回失败，不能静默
    CHECK(!sim.ep[0].tx.resend(42, sim.now));
    CHECK(!sim.ep[0].tx.resend(0, sim.now));

    CHECK(sim.try_submit(1));
    CHECK(sim.ep[0].tx.resend(0, sim.now));  // 在窗内：可重放
    CHECK(!sim.ep[0].tx.resend(1, sim.now)); // 尚未分配：不可重放
}

void test_reorder_store()
{
    // 乱序载荷存储：槽位由 seq % slot_count 决定，存、取、释放、复位各司其职
    u8 storage[static_cast<size_t>(MAX_WINDOW) * 8] = {};
    ReorderStore store;
    store.init(storage, 8, MAX_WINDOW);

    const u8 a[4] = {1, 2, 3, 4};
    u16 len = 0;

    CHECK(store.get(3, &len) == nullptr); // 没存过：取不到
    store.store(3, a, sizeof(a));
    const u8 *got = store.get(3, &len);
    CHECK(got != nullptr);
    CHECK_EQ(len, sizeof(a));
    CHECK(std::memcmp(got, a, sizeof(a)) == 0);
    CHECK(got == storage + 3 * 8); // 落在第 3 个槽

    // 槽位按 seq % slot_count 复用：回绕到同一槽的序号读到的是新载荷，调用方靠
    // "在途距离 < slot_count"避免这种混淆
    const u8 b[2] = {9, 8};
    store.store(3 + MAX_WINDOW, b, sizeof(b));
    got = store.get(3 + MAX_WINDOW, &len);
    CHECK(got != nullptr);
    CHECK_EQ(len, sizeof(b));
    CHECK(got == storage + 3 * 8);

    store.release(3 + MAX_WINDOW);
    CHECK(store.get(3 + MAX_WINDOW, &len) == nullptr);

    store.store(5, a, sizeof(a));
    store.reset();
    CHECK(store.get(5, &len) == nullptr);

    // 载荷超过槽容量：按槽容量截断，不越界写
    u8 small[4] = {};
    ReorderStore tiny;
    tiny.init(small, 2, 2);
    const u8 big[5] = {1, 2, 3, 4, 5};
    tiny.store(0, big, sizeof(big));
    got = tiny.get(0, &len);
    CHECK(got != nullptr);
    CHECK_EQ(len, 2u);
    CHECK_EQ(got[0], 1u);
    CHECK_EQ(got[1], 2u);

    // 槽数越界收敛到 MAX_WINDOW：槽偏移按收敛后的槽数算
    u8 wide[static_cast<size_t>(MAX_WINDOW) * 4] = {};
    ReorderStore clamped;
    clamped.init(wide, 4, MAX_WINDOW + 4);
    clamped.store(MAX_WINDOW + 1, a, sizeof(a));
    got = clamped.get(MAX_WINDOW + 1, &len);
    CHECK(got != nullptr);
    CHECK(got == wide + 1 * 4); // 第 (MAX_WINDOW + 1) % MAX_WINDOW 个槽
}

void test_receiver_pairs_with_reorder_store()
{
    // 窗口大于 1 的接收路径：乱序帧先进缓冲，缺的那条到达时一次补齐整段
    Config cfg;
    cfg.window = 8;
    cfg.reorder = true;

    Receiver rx;
    rx.init(cfg);
    rx.reset(0);

    u8 storage[static_cast<size_t>(MAX_WINDOW) * 16] = {};
    ReorderStore store;
    store.init(storage, 16, MAX_WINDOW);

    const u8 p1[3] = {11, 11, 11};
    const u8 p2[3] = {22, 22, 22};

    // 先到两条乱序帧：只进缓冲，交付游标不动
    CHECK(rx.on_frame(1, false, 0) == RxResult::Buffered);
    store.store(1, p1, sizeof(p1));
    CHECK(rx.on_frame(2, false, 0) == RxResult::Buffered);
    store.store(2, p2, sizeof(p2));
    CHECK_EQ(rx.ack(), 0u);
    CHECK(rx.sack_bitmap() != 0u);

    // 缺的那条到了：一次 commit 把 0..2 整段推出去
    CHECK(rx.on_frame(0, false, 0) == RxResult::Delivered);
    const u16 last = rx.commit(0, 0);
    CHECK_EQ(last, 2u);

    // 区间里 1、2 的载荷都能从缓冲里取出来，顺序与长度都对
    u16 len = 0;
    const u8 *one = store.get(1, &len);
    CHECK(one != nullptr);
    CHECK_EQ(len, sizeof(p1));
    CHECK(std::memcmp(one, p1, sizeof(p1)) == 0);
    const u8 *two = store.get(2, &len);
    CHECK(two != nullptr);
    CHECK_EQ(len, sizeof(p2));
    CHECK(std::memcmp(two, p2, sizeof(p2)) == 0);
    CHECK_EQ(rx.ack(), 3u);

    // 交付完的槽释放后不再有效
    store.release(1);
    store.release(2);
    CHECK(store.get(1, &len) == nullptr);
}

struct TestCase
{
    const char *name;
    void (*fn)();
};

const TestCase kTests[] = {
    {"模比较与 SACK 编码", test_helpers},
    {"无丢包基线", test_clean_run},
    {"丢帧链路（数据 + ACK）", test_lossy_link},
    {"MAC 发送失败提前重传", test_mac_failure_fast_retransmit},
    {"重复帧不重复交付", test_duplicate_frames_never_repeat_delivery},
    {"乱序 + SACK 按序交付", test_reorder_with_sack},
    {"关闭重排退化为重传补齐", test_reorder_disabled_falls_back_to_retransmit},
    {"会话起始丢帧不丢消息", test_session_start_with_lost_frames},
    {"窗口 8 流水线", test_window_pipelining},
    {"序号回绕 70000 条", test_sequence_wraparound},
    {"重传预算与失败时刻", test_failure_budget},
    {"熔断与恢复", test_breaker_and_recovery},
    {"放弃后空洞可跳过、流继续走", test_gap_skip_after_give_up},
    {"对端窗口为 0 时流控", test_flow_control_blocks_on_zero_window},
    {"重放与未命中上报", test_replay_and_miss},
    {"乱序载荷存储的槽位与截断", test_reorder_store},
    {"接收端与乱序缓冲联动补齐", test_receiver_pairs_with_reorder_store},
};
} // namespace

int main()
{
    for (const TestCase &test : kTests)
    {
        const int before = g_fail;
        std::print("== {}\n", test.name);
        test.fn();
        std::print("   {}\n", (g_fail == before) ? "通过" : "失败");
    }

    std::print("\n断言：{} 通过 / {} 失败\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
