#include "meguminlink/wireless/arq.hpp"

#include <cstring>

using namespace mlink;
using namespace mlink::arq;

// ---- RttEstimator ----

void RttEstimator::init(u16 min_ms, u16 max_ms, u16 initial_ms)
{
    m_min_ms = min_ms;
    m_max_ms = max_ms;
    m_initial_ms = (initial_ms < min_ms) ? min_ms : ((initial_ms > max_ms) ? max_ms : initial_ms);
    m_srtt_ms = 0;
    m_rttvar_ms = 0;
    m_have_sample = false;
}

void RttEstimator::update(u32 sample_ms)
{
    if (!m_have_sample)
    {
        // 第一个样本直接定基准：偏差取样本的一半（Jacobson 的朴素起点）
        m_srtt_ms = sample_ms;
        m_rttvar_ms = sample_ms / 2;
        m_have_sample = true;
        return;
    }

    const u32 diff = (m_srtt_ms > sample_ms) ? (m_srtt_ms - sample_ms) : (sample_ms - m_srtt_ms);
    m_rttvar_ms = (3 * m_rttvar_ms + diff) / 4;
    m_srtt_ms = (7 * m_srtt_ms + sample_ms) / 8;
}

u32 RttEstimator::base_ms() const
{
    u32 rto = m_have_sample ? (m_srtt_ms + 4 * m_rttvar_ms) : m_initial_ms;
    if (rto < m_min_ms)
    {
        rto = m_min_ms;
    }
    if (rto > m_max_ms)
    {
        rto = m_max_ms;
    }
    return rto;
}

u32 RttEstimator::rto_ms(u8 shift) const
{
    if (shift > MAX_BACKOFF_SHIFT)
    {
        shift = MAX_BACKOFF_SHIFT;
    }

    // 基础值 ≤ u16 上限、shift ≤ 8，左移结果仍在 u32 内，随后按上限截断
    u32 rto = base_ms() << shift;
    if (rto > m_max_ms)
    {
        rto = m_max_ms;
    }
    if (rto < m_min_ms)
    {
        rto = m_min_ms;
    }
    return rto;
}

// ---- Breaker ----

void Breaker::init(u8 threshold, u16 probe_ms)
{
    m_threshold = threshold;
    m_probe_ms = probe_ms;
    m_consecutive = 0;
    m_tripped = false;
    m_trips = 0;
    m_probe_at_ms = 0;
}

bool Breaker::allow(u32 now_ms)
{
    if (!m_tripped || m_threshold == 0)
    {
        return true;
    }
    if (static_cast<i32>(now_ms - m_probe_at_ms) < 0)
    {
        return false; // 未到试探时刻：熔断期间的提交一律快速失败
    }
    m_probe_at_ms = now_ms + m_probe_ms;
    return true;
}

bool Breaker::on_failure(u32 now_ms)
{
    if (m_threshold == 0)
    {
        return false;
    }

    if (m_consecutive < 0xFF)
    {
        m_consecutive++;
    }
    if (m_tripped || m_consecutive < m_threshold)
    {
        return false;
    }

    m_tripped = true;
    m_trips++;
    m_probe_at_ms = now_ms + m_probe_ms;
    return true;
}

bool Breaker::on_success()
{
    m_consecutive = 0;
    const bool was_tripped = m_tripped;
    m_tripped = false;
    return was_tripped;
}

// ---- Sender ----

void Sender::init(const Config &config, u8 *storage, size_t slot_cap, size_t slot_count)
{
    m_config = config;
    if (m_config.window == 0)
    {
        m_config.window = 1;
    }
    if (m_config.window > MAX_WINDOW)
    {
        m_config.window = MAX_WINDOW;
    }
    if (m_config.rto_min_ms == 0)
    {
        m_config.rto_min_ms = 1; // 到期时刻用 0 表示"无待办"，下限不能是 0
    }
    if (m_config.rto_max_ms < m_config.rto_min_ms)
    {
        m_config.rto_max_ms = m_config.rto_min_ms;
    }

    m_storage = storage;
    m_slot_cap = slot_cap;

    m_slot_count = (slot_count > MAX_WINDOW) ? MAX_WINDOW : slot_count;
    if (m_slot_count == 0)
    {
        m_slot_count = 1;
    }

    m_tx_frames = 0;
    m_retx_frames = 0;
    m_acked_frames = 0;
    m_failed_frames = 0;

    reset();
}

void Sender::reset()
{
    for (Slot &slot : m_slots)
    {
        slot = Slot{};
    }

    m_tx_base = 0;
    m_tx_next = 0;
    m_mac_failed = false;
    m_abandoned = false;
    m_peer_window = MAX_WINDOW;

    m_rtt.init(m_config.rto_min_ms, m_config.rto_max_ms, m_config.initial_rto_ms);
    m_breaker.init(m_config.breaker_failures, m_config.breaker_probe_ms);
}

void Sender::set_callbacks(void *ctx, TransmitFn transmit, EventFn event)
{
    m_ctx = ctx;
    m_transmit = transmit;
    m_event = event;
}

Sender::Slot &Sender::slot_of(u16 seq)
{
    return m_slots[seq % m_slot_count];
}

const Sender::Slot &Sender::slot_of(u16 seq) const
{
    return m_slots[seq % m_slot_count];
}

u8 *Sender::payload_of(u16 seq)
{
    return m_storage + (seq % m_slot_count) * m_slot_cap;
}

const u8 *Sender::payload_of(u16 seq) const
{
    return m_storage + (seq % m_slot_count) * m_slot_cap;
}

void Sender::emit(Event event, u16 seq, u32 now_ms) const
{
    if (m_event != nullptr)
    {
        m_event(m_ctx, event, seq, now_ms);
    }
}

u8 Sender::window() const
{
    return (m_peer_window < m_config.window) ? m_peer_window : m_config.window;
}

SubmitResult Sender::submit(const u8 *payload, u16 len, u32 now_ms, u16 *seq_out)
{
    if (payload == nullptr || m_storage == nullptr || len == 0 || len > m_slot_cap)
    {
        return SubmitResult::Invalid;
    }
    if (in_flight() >= window())
    {
        return SubmitResult::WindowFull;
    }
    if (!m_breaker.allow(now_ms))
    {
        return SubmitResult::BreakerOpen;
    }

    const u16 seq = m_tx_next;
    Slot &slot = slot_of(seq);

    std::memcpy(payload_of(seq), payload, len);
    slot.seq = seq;
    slot.len = len;
    slot.retx = 0;
    slot.backoff_shift = 0;
    slot.used = true;
    slot.ever_retx = false;
    slot.first_tx_ms = now_ms;
    slot.earliest_retx_ms = now_ms + MIN_RETX_INTERVAL_MS;
    slot.deadline_ms = now_ms + m_rtt.rto_ms(0);

    m_tx_next = static_cast<u16>(seq + 1);
    m_tx_frames++;

    emit(Event::Submitted, seq, now_ms);
    if (m_transmit != nullptr)
    {
        // gap_before 只在"本帧就是最老未确认帧"时置位：此时它之前的序号要么已
        // 确认、要么已被放弃，跳过去不会误伤任何仍在途的帧
        m_transmit(m_ctx, seq, payload_of(seq), len, false, m_abandoned && seq == m_tx_base);
    }

    if (seq_out != nullptr)
    {
        *seq_out = seq;
    }
    return SubmitResult::Ok;
}

bool Sender::resend(u16 seq, u32 now_ms)
{
    if (idle() || seq_before(seq, m_tx_base) || !seq_before(seq, m_tx_next))
    {
        return false;
    }

    const Slot &slot = slot_of(seq);
    if (!slot.used || slot.seq != seq)
    {
        return false;
    }

    retransmit(seq, now_ms);
    return true;
}

bool Sender::on_ack(u16 ack, u16 sack_bitmap, u8 peer_window, u32 now_ms)
{
    m_peer_window = (peer_window > MAX_WINDOW) ? MAX_WINDOW : peer_window;

    bool any_new = false;
    bool sampled = false;
    const u16 base = m_tx_base;
    const u16 count = in_flight();

    for (u16 i = 0; i < count; ++i)
    {
        const u16 seq = static_cast<u16>(base + i);
        Slot &slot = slot_of(seq);
        if (!slot.used)
        {
            continue;
        }

        const bool cumulative = seq_before(seq, ack);
        const bool selective = !cumulative && sack_test(sack_bitmap, ack, seq);
        if (!cumulative && !selective)
        {
            continue;
        }

        // 一个确认只喂一个 RTT 样本（取最老的那条），否则窗口一大就把估计值带偏；
        // 重传过的消息不采样（Karn）
        if (!slot.ever_retx && !sampled)
        {
            m_rtt.update(now_ms - slot.first_tx_ms);
            sampled = true;
        }

        slot.used = false;
        m_acked_frames++;
        any_new = true;
        emit(Event::Acked, seq, now_ms);
    }

    // 窗口基准只沿连续确认推进：SACK 补掉的是空洞，基准要停在真正的缺口上
    while (m_tx_base != m_tx_next && !slot_of(m_tx_base).used)
    {
        m_tx_base = static_cast<u16>(m_tx_base + 1);
    }

    if (any_new)
    {
        m_mac_failed = false; // 有确认回来，之前的"没发出去"提示作废
        if (m_breaker.on_success())
        {
            emit(Event::BreakerReset, m_tx_base, now_ms);
        }
    }
    return any_new;
}

void Sender::on_mac_failure(u32 now_ms)
{
    m_mac_failed = true;

    // 让最早重传时刻不早于失败回执之后 MIN_RETX_INTERVAL_MS：一帧刚发出去就
    // 拿到失败回执时，避免连着几次失败在几毫秒内烧光重传预算
    if (idle())
    {
        return;
    }
    Slot &slot = slot_of(m_tx_base);
    const u32 floor_ms = now_ms + MIN_RETX_INTERVAL_MS;
    if (slot.used && static_cast<i32>(floor_ms - slot.earliest_retx_ms) > 0)
    {
        slot.earliest_retx_ms = floor_ms;
    }
}

void Sender::retransmit(u16 seq, u32 now_ms)
{
    Slot &slot = slot_of(seq);
    if (!slot.used || slot.seq != seq)
    {
        return;
    }
    if (slot.retx >= m_config.max_retx)
    {
        fail(seq, now_ms);
        return;
    }

    slot.retx++;
    slot.ever_retx = true;
    if (slot.backoff_shift < MAX_BACKOFF_SHIFT)
    {
        slot.backoff_shift++;
    }
    slot.earliest_retx_ms = now_ms + MIN_RETX_INTERVAL_MS;
    slot.deadline_ms = now_ms + m_rtt.rto_ms(slot.backoff_shift);
    m_retx_frames++;

    emit(Event::Retransmit, seq, now_ms);
    if (m_transmit != nullptr)
    {
        m_transmit(m_ctx, seq, payload_of(seq), slot.len, true, m_abandoned && seq == m_tx_base);
    }
}

void Sender::fail(u16 seq, u32 now_ms)
{
    slot_of(seq).used = false;
    m_failed_frames++;
    m_abandoned = true;
    emit(Event::Failed, seq, now_ms);

    while (m_tx_base != m_tx_next && !slot_of(m_tx_base).used)
    {
        m_tx_base = static_cast<u16>(m_tx_base + 1);
    }

    if (m_breaker.on_failure(now_ms))
    {
        emit(Event::BreakerTrip, seq, now_ms);
    }
}

void Sender::poll(u32 now_ms)
{
    // 1) MAC 发送失败提示：合并成一次提前重传，不等 RTO
    if (m_mac_failed && !idle())
    {
        Slot &slot = slot_of(m_tx_base);
        if (slot.used && static_cast<i32>(now_ms - slot.earliest_retx_ms) >= 0)
        {
            m_mac_failed = false;
            retransmit(m_tx_base, now_ms);
        }
    }

    // 2) RTO 到期。基准与条数先取定：重传可能把消息判死并推进基准，
    //    按绝对序号遍历才不会漏帧或重复处理。
    const u16 base = m_tx_base;
    const u16 count = in_flight();
    for (u16 i = 0; i < count; ++i)
    {
        const u16 seq = static_cast<u16>(base + i);
        Slot &slot = slot_of(seq);
        if (!slot.used)
        {
            continue;
        }
        if (static_cast<i32>(now_ms - slot.deadline_ms) >= 0)
        {
            retransmit(seq, now_ms);
        }
    }
}

u32 Sender::next_deadline_ms() const
{
    u32 best = 0;
    const u16 base = m_tx_base;
    const u16 count = in_flight();

    for (u16 i = 0; i < count; ++i)
    {
        const Slot &slot = slot_of(static_cast<u16>(base + i));
        if (!slot.used)
        {
            continue;
        }

        u32 due = slot.deadline_ms;
        if (m_mac_failed && i == 0 && slot.earliest_retx_ms < due)
        {
            due = slot.earliest_retx_ms;
        }

        // 0 是"没有待办"的哨兵：已提交消息的到期时刻必然大于 0（RTO 下限 ≥ 1ms）
        if (best == 0 || static_cast<i32>(due - best) < 0)
        {
            best = due;
        }
    }
    return best;
}

SenderSnapshot Sender::snapshot() const
{
    SenderSnapshot out = {};
    out.tx_frames = m_tx_frames;
    out.retx_frames = m_retx_frames;
    out.acked_frames = m_acked_frames;
    out.failed_frames = m_failed_frames;
    out.breaker_trips = m_breaker.trips();
    out.srtt_ms = m_rtt.srtt_ms();
    out.rttvar_ms = m_rtt.rttvar_ms();
    out.rto_ms = m_rtt.base_ms();
    out.in_flight = in_flight();
    out.window = window();
    out.breaker_open = m_breaker.tripped();
    return out;
}

// ---- Receiver ----

void Receiver::init(const Config &config)
{
    m_config = config;
    if (m_config.window == 0)
    {
        m_config.window = 1;
    }
    if (m_config.window > MAX_WINDOW)
    {
        m_config.window = MAX_WINDOW;
    }

    m_delivered = 0;
    m_buffered = 0;
    m_duplicate = 0;
    m_out_of_window = 0;
    m_skipped = 0;

    reset(0);
}

void Receiver::reset(u16 initial_seq)
{
    m_rx_next = initial_seq;
    m_received = 0;
    m_ack_pending = false;
}

void Receiver::set_callbacks(void *ctx, EventFn event)
{
    m_ctx = ctx;
    m_event = event;
}

void Receiver::emit(Event event, u16 seq, u32 now_ms) const
{
    if (m_event != nullptr)
    {
        m_event(m_ctx, event, seq, now_ms);
    }
}

RxResult Receiver::on_frame(u16 seq, bool gap_before, u32 now_ms)
{
    // 对端声明放弃了本序号之前的空洞：游标直接跳过来，中间未交付的帧按丢失处理。
    // 这是流从"某条消息重传耗尽"里恢复的唯一手段——不跳的话游标会永远卡在空洞
    // 上，之后每一帧都被判成超窗，整条链路僵死。只接受向前的跳转：迟到的旧标记
    // 帧绝不能把游标倒回去。
    if (gap_before && seq_before(m_rx_next, seq))
    {
        const u16 rel = seq_diff(seq, m_rx_next);
        if (rel < static_cast<u16>(SACK_BITS))
        {
            m_received = static_cast<u16>(m_received >> rel);
            m_received = static_cast<u16>(m_received & ~1u); // 本帧自身按新到处理
        }
        else
        {
            // 跳过的距离超出位图能表达的范围：位图整体作废
            m_received = 0;
        }
        m_skipped += rel;
        m_rx_next = seq;
    }

    if (seq == m_rx_next)
    {
        m_ack_pending = true;
        return RxResult::Delivered;
    }

    if (seq_before(seq, m_rx_next))
    {
        m_ack_pending = true; // 再确认一次：让对端停止重传这一帧
        m_duplicate++;
        emit(Event::Duplicate, seq, now_ms);
        return RxResult::Duplicate;
    }

    // seq 在游标之后。可缓冲的距离必须小于窗口：rel == window 的那一帧与游标
    // 落在同一个槽上，游标补齐时会把还没交付的乱序载荷覆盖掉。
    const u16 rel = seq_diff(seq, m_rx_next);
    if (m_config.reorder && rel < m_config.window)
    {
        const u16 bit = static_cast<u16>(1u << (rel - 1));
        if ((m_received & bit) != 0)
        {
            m_ack_pending = true;
            m_duplicate++;
            emit(Event::Duplicate, seq, now_ms);
            return RxResult::Duplicate;
        }

        m_received = static_cast<u16>(m_received | bit);
        m_ack_pending = true;
        m_buffered++;
        emit(Event::Buffered, seq, now_ms);
        return RxResult::Buffered;
    }

    // 超窗（或本端不支持重排）：不标记、不确认，等对端超时重传
    m_out_of_window++;
    return RxResult::OutOfWindow;
}

u16 Receiver::commit(u16 seq, u32 now_ms)
{
    m_rx_next = static_cast<u16>(seq + 1);
    m_delivered++;
    emit(Event::Delivered, seq, now_ms);

    // 位图的第 i 位表示 seq == 游标 + 1 + i：游标每前移一格，基准就右移一格，
    // 而"新游标处的帧是否已收到"正是右移前的第 0 位
    u16 last = seq;
    for (;;)
    {
        const bool cursor_buffered = (m_received & 1u) != 0;
        m_received = static_cast<u16>(m_received >> 1);
        if (!cursor_buffered)
        {
            break;
        }

        last = m_rx_next;
        m_rx_next = static_cast<u16>(m_rx_next + 1);
        m_delivered++;
        emit(Event::Delivered, last, now_ms);
    }
    return last;
}

u8 Receiver::window() const
{
    if (!m_config.reorder)
    {
        // 不支持重排时照常通告完整窗口：乱序帧会被判为 OutOfWindow 并由对端超时
        // 重传（即 go-back-N），通告 1 会把链路直接退化成停等，白白丢掉窗口本来
        // 能掩盖的往返延迟
        return m_config.window;
    }

    u8 used = 0;
    for (u16 bits = m_received; bits != 0; bits = static_cast<u16>(bits >> 1))
    {
        used = static_cast<u8>(used + (bits & 1u));
    }
    return (used >= m_config.window) ? 0 : static_cast<u8>(m_config.window - used);
}

ReceiverSnapshot Receiver::snapshot() const
{
    ReceiverSnapshot out = {};
    out.delivered = m_delivered;
    out.buffered = m_buffered;
    out.duplicate = m_duplicate;
    out.out_of_window = m_out_of_window;
    out.skipped = m_skipped;
    out.ack = m_rx_next;
    out.sack = m_received;
    out.window = window();
    return out;
}
