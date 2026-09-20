#include "meguminlink/arq_link.hpp"

#include <cstring>

#include <esp_log.h>
#include <esp_random.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::arq_link";

// ---- 构造与配置 ----

esp_err_t ArqLink::init(const arq::Config &config, size_t payload_cap)
{
    if (payload_cap == 0 || payload_cap > MAX_PAYLOAD)
    {
        return ESP_ERR_INVALID_ARG;
    }

    m_config = config;
    m_payload_cap = payload_cap;

    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    m_sender.init(m_config, m_tx_storage, m_payload_cap, arq::MAX_WINDOW);
    m_receiver.init(m_config);
    m_sender.set_callbacks(this, &ArqLink::core_transmit, &ArqLink::core_event);
    m_receiver.set_callbacks(this, &ArqLink::core_event);

    reset_session(0);
    return ESP_OK;
}

void ArqLink::set_callbacks(const Callbacks &callbacks)
{
    lock();
    m_callbacks = callbacks;
    unlock();
}

void ArqLink::lock() const
{
    xSemaphoreTake(m_mutex, portMAX_DELAY);
}

void ArqLink::unlock() const
{
    xSemaphoreGive(m_mutex);
}

void ArqLink::queue_locked(const Action &action)
{
    if (m_action_count >= MAX_ACTIONS)
    {
        // 队列深度按"一次 poll 最多产生几条帧"预留：正常到不了这里
        ESP_LOGW(TAG, "action queue full, dropping kind=%d", static_cast<int>(action.kind));
        return;
    }
    m_actions[m_action_count++] = action;
}

size_t ArqLink::take_actions_locked(Action *out)
{
    const size_t count = m_action_count;
    for (size_t i = 0; i < count; ++i)
    {
        out[i] = m_actions[i];
    }
    m_action_count = 0;
    return count;
}

void ArqLink::dispatch(const Action *actions, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        const Action &action = actions[i];
        switch (action.kind)
        {
        case Action::Kind::Message: {
            if (m_callbacks.transmit == nullptr)
            {
                break;
            }
            // 载荷按序号定址（槽布局固定），因此出锁后取也安全：这段时间里不会
            // 有新消息提交——提交只发生在同一个任务里
            const u8 *payload = m_tx_storage + (action.seq % arq::MAX_WINDOW) * m_payload_cap;
            m_callbacks.transmit(m_callbacks.ctx, action.seq, payload, action.len, action.retransmit,
                                 action.gap_before, action.ack, action.sack, action.window);
            break;
        }
        case Action::Kind::Ack: {
            if (m_callbacks.send_ack != nullptr)
            {
                m_callbacks.send_ack(m_callbacks.ctx, action.ack, action.sack, action.window);
            }
            break;
        }
        case Action::Kind::Reset: {
            if (m_callbacks.send_reset != nullptr)
            {
                m_callbacks.send_reset(m_callbacks.ctx, action.epoch, action.ack, action.sack, action.window);
            }
            break;
        }
        }
    }
}

void ArqLink::set_peer(const u8 *peer_mac, u32 now_ms)
{
    if (peer_mac == nullptr)
    {
        return;
    }

    lock();
    const bool changed = !m_peer_known || std::memcmp(m_peer_mac, peer_mac, sizeof(m_peer_mac)) != 0;
    std::memcpy(m_peer_mac, peer_mac, sizeof(m_peer_mac));
    m_peer_known = true;
    unlock();

    // 对端换了就必须重新对齐序号基准；同一个对端重复调用是幂等的
    if (changed)
    {
        reset_session(now_ms);
    }
}

void ArqLink::clear_peer()
{
    lock();
    m_peer_known = false;
    m_established = false;
    m_sender.reset();
    m_receiver.reset(0);
    m_ack_scheduled = false;
    m_replay_pending = false;
    m_reset_reply_pending = false;
    m_action_count = 0;
    unlock();
}

bool ArqLink::has_peer() const
{
    lock();
    const bool known = m_peer_known;
    unlock();
    return known;
}

bool ArqLink::established() const
{
    lock();
    const bool value = m_established;
    unlock();
    return value;
}

bool ArqLink::ack_pending() const
{
    lock();
    const bool value = m_ack_scheduled;
    unlock();
    return value;
}

ArqLink::Piggyback ArqLink::take_piggyback()
{
    lock();

    Piggyback out = {};
    out.ack = m_receiver.ack();
    out.sack = m_receiver.sack_bitmap();
    out.window = m_receiver.window();

    // 这一帧带着确认出去了，确认义务随之完成
    m_receiver.clear_ack_pending();
    m_ack_scheduled = false;

    unlock();
    return out;
}

bool ArqLink::peer_mac(u8 *out) const
{
    lock();
    const bool known = m_peer_known;
    if (known && out != nullptr)
    {
        std::memcpy(out, m_peer_mac, sizeof(m_peer_mac));
    }
    unlock();
    return known;
}

void ArqLink::reset_session(u32 now_ms)
{
    lock();
    m_sender.reset();
    m_receiver.reset(0);
    m_established = false;
    m_ack_scheduled = false;
    m_replay_pending = false;
    m_reset_reply_pending = false;
    m_action_count = 0;
    m_peer_epoch_known = false;
    std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));

    // 换一个 epoch：对端据此把"本端重启后的新会话"与"旧会话的迟到帧"分开
    m_epoch = esp_random() | 1u;
    m_next_reset_tx_ms = now_ms;
    unlock();
}

// ---- 接收路径（ESP-NOW 回调上下文：只更新状态与待发标记）----

void ArqLink::on_ack(u16 ack, u16 sack, u8 window, u32 now_ms)
{
    lock();
    m_sender.on_ack(ack, sack, window, now_ms);
    unlock();
}

void ArqLink::on_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u32 now_ms)
{
    lock();

    const arq::RxResult result = m_receiver.on_frame(seq, gap_before, now_ms);
    if (result == arq::RxResult::Delivered)
    {
        // 重排关闭时 commit 只推进一帧，手里这条载荷就是要交付的那条
        const u16 last = m_receiver.commit(seq, now_ms);
        if (last != seq)
        {
            // 打开重排才可能出现：那时交付的是缓冲区里的后继帧，本层没有它的载荷
            ESP_LOGW(TAG, "reorder not supported here (commit %u..%u)", seq, last);
        }
        if (m_callbacks.deliver != nullptr)
        {
            u8 peer_mac[6] = {};
            if (m_peer_known)
            {
                std::memcpy(peer_mac, m_peer_mac, sizeof(peer_mac));
            }
            m_callbacks.deliver(m_callbacks.ctx, m_peer_known ? peer_mac : nullptr, seq, payload, len);
        }
    }
    else if (result == arq::RxResult::Duplicate)
    {
        // 对端在重传：记下来，poll 里重放缓存响应（绝不再执行一次）
        m_replay_pending = true;
        m_replay_cmd_seq = seq;
    }

    if (m_receiver.ack_pending() && !m_ack_scheduled)
    {
        m_ack_scheduled = true;
        m_ack_due_ms = now_ms + m_config.delayed_ack_ms;
    }

    unlock();
}

void ArqLink::on_reset(u32 peer_epoch, u32 now_ms)
{
    lock();

    if (!m_peer_epoch_known || m_peer_epoch != peer_epoch)
    {
        // 对端开了一个新会话（上电或链路重建）：本端把状态拉到同一序号基准
        m_peer_epoch = peer_epoch;
        m_peer_epoch_known = true;
        m_sender.reset();
        m_receiver.reset(0);
        m_ack_scheduled = false;
        m_replay_pending = false;
        std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));
        m_established = true;
        ESP_LOGI(TAG, "peer session epoch 0x%08X accepted, sequence space realigned", peer_epoch);
    }
    else if (!m_established)
    {
        m_established = true;
    }

    // 无论是否新会话都要回一个自己的 epoch，对端据此完成握手。这里是 ESP-NOW
    // 接收回调的上下文，只能登记，真正的发送在 poll 里做。
    m_reset_reply_pending = true;
    if (static_cast<i32>(now_ms - m_reset_reply_due_ms) >= 0)
    {
        m_reset_reply_due_ms = now_ms + RESET_REPLY_MIN_MS;
    }

    unlock();
}

// ---- 任务上下文 ----

void ArqLink::poll(u32 now_ms)
{
    Action actions[MAX_ACTIONS];

    lock();

    // 1) 会话未对齐：周期性发重置请求，直到收到对端的 epoch
    if (!m_established && m_peer_known && static_cast<i32>(now_ms - m_next_reset_tx_ms) >= 0)
    {
        queue_locked(make_reset_action_locked());
        m_next_reset_tx_ms = now_ms + RESET_INTERVAL_MS;
    }

    // 1b) 对端请求重置的回显（限速）
    if (m_reset_reply_pending && static_cast<i32>(now_ms - m_reset_reply_due_ms) >= 0)
    {
        queue_locked(make_reset_action_locked());
        m_reset_reply_pending = false;
    }

    // 2) 重复命令触发的响应重放
    if (m_replay_pending)
    {
        m_replay_pending = false;
        const size_t slot = m_replay_cmd_seq % arq::MAX_WINDOW;
        if (m_replay_map_valid[slot] && m_sender.resend(m_replay_map[slot], now_ms))
        {
            m_replayed++;
        }
        else
        {
            // 响应已被确认（对端其实收到了）或还没产生（命令仍在执行）：只能等
            m_replay_missed++;
        }
    }

    // 3) RTO 到期与 MAC 失败触发的重传
    m_sender.poll(now_ms);

    // 4) 攒够时间的纯确认
    queue_ack_locked(now_ms);

    const size_t count = take_actions_locked(actions);
    unlock();

    dispatch(actions, count);
}

ArqLink::Action ArqLink::make_reset_action_locked() const
{
    Action action = {};
    action.kind = Action::Kind::Reset;
    action.epoch = m_epoch;
    action.ack = m_receiver.ack();
    action.sack = m_receiver.sack_bitmap();
    action.window = m_receiver.window();
    return action;
}

void ArqLink::queue_ack_locked(u32 now_ms)
{
    if (!m_ack_scheduled || static_cast<i32>(now_ms - m_ack_due_ms) < 0)
    {
        return;
    }

    Action action = {};
    action.kind = Action::Kind::Ack;
    action.ack = m_receiver.ack();
    action.sack = m_receiver.sack_bitmap();
    action.window = m_receiver.window();
    queue_locked(action);

    m_receiver.clear_ack_pending();
    m_ack_scheduled = false;
}

u32 ArqLink::next_deadline_ms() const
{
    lock();

    u32 best = m_sender.next_deadline_ms();

    if (!m_established && m_peer_known)
    {
        if (best == 0 || static_cast<i32>(m_next_reset_tx_ms - best) < 0)
        {
            best = m_next_reset_tx_ms;
        }
    }
    if (m_ack_scheduled && (best == 0 || static_cast<i32>(m_ack_due_ms - best) < 0))
    {
        best = m_ack_due_ms;
    }
    if (m_reset_reply_pending && (best == 0 || static_cast<i32>(m_reset_reply_due_ms - best) < 0))
    {
        best = m_reset_reply_due_ms;
    }

    unlock();
    return best;
}

ArqLink::Submit ArqLink::submit(const u8 *payload, u16 len, u32 now_ms, u16 *seq_out)
{
    Action actions[MAX_ACTIONS];

    lock();
    m_action_count = 0;

    Submit result = Submit::NotEstablished;
    if (m_peer_known && m_established)
    {
        switch (m_sender.submit(payload, len, now_ms, seq_out))
        {
        case arq::SubmitResult::Ok:
            result = Submit::Ok;
            break;
        case arq::SubmitResult::WindowFull:
            result = Submit::WindowFull;
            break;
        case arq::SubmitResult::BreakerOpen:
            result = Submit::BreakerOpen;
            break;
        default:
            result = Submit::Invalid;
            break;
        }
    }

    const size_t count = take_actions_locked(actions);
    unlock();

    // 首传是核心在 submit 内部回调登记的，出锁后再真正发出去
    dispatch(actions, count);
    return result;
}

ArqLink::Submit ArqLink::submit_response(u16 cmd_seq, const u8 *payload, u16 len, u32 now_ms)
{
    u16 resp_seq = 0;
    const Submit result = submit(payload, len, now_ms, &resp_seq);
    if (result != Submit::Ok)
    {
        return result;
    }

    // 记下"这条响应回的是哪条命令"：对端重传该命令时据此重放，不必重新执行
    lock();
    const size_t slot = cmd_seq % arq::MAX_WINDOW;
    m_replay_map[slot] = resp_seq;
    m_replay_map_valid[slot] = true;
    unlock();

    return result;
}

ArqLink::Snapshot ArqLink::snapshot() const
{
    lock();

    const arq::SenderSnapshot tx = m_sender.snapshot();
    const arq::ReceiverSnapshot rx = m_receiver.snapshot();

    Snapshot out = {};
    out.tx = tx.tx_frames;
    out.retx = tx.retx_frames;
    out.acked = tx.acked_frames;
    out.failed = tx.failed_frames;
    out.rx_delivered = rx.delivered;
    out.rx_duplicate = rx.duplicate;
    out.rx_out_of_window = rx.out_of_window;
    out.rx_skipped = rx.skipped;
    out.replayed = m_replayed;
    out.replay_missed = m_replay_missed;
    out.breaker_trips = tx.breaker_trips;
    out.srtt_ms = tx.srtt_ms;
    out.rto_ms = tx.rto_ms;
    out.in_flight = tx.in_flight;
    out.established = m_established;
    out.breaker_open = tx.breaker_open;

    unlock();
    return out;
}

// ---- 核心回调 ----

void ArqLink::core_transmit(void *raw, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before)
{
    (void)payload;
    auto *self = static_cast<ArqLink *>(raw);

    // 本函数在核心持锁时被调用：只登记动作，出锁后才发送（锁内调 WiFi 驱动会死锁）
    Action action = {};
    action.kind = Action::Kind::Message;
    action.seq = seq;
    action.len = len;
    action.retransmit = retransmit;
    action.gap_before = gap_before;
    action.ack = self->m_receiver.ack();
    action.sack = self->m_receiver.sack_bitmap();
    action.window = self->m_receiver.window();
    self->queue_locked(action);

    // 数据帧捎带确认即完成确认义务
    self->m_receiver.clear_ack_pending();
    self->m_ack_scheduled = false;
}

void ArqLink::core_event(void *raw, arq::Event event, u16 seq, u32 now_ms)
{
    auto *self = static_cast<ArqLink *>(raw);
    (void)now_ms;

    // 计数一律从核心快照读，这里只负责把需要人看的事件打到日志
    switch (event)
    {
    case arq::Event::Retransmit: {
        const arq::SenderSnapshot snap = self->m_sender.snapshot();
        ESP_LOGI(TAG, "retransmit seq=%u srtt=%u rttvar=%u rto=%u", seq, snap.srtt_ms, snap.rttvar_ms, snap.rto_ms);
        break;
    }
    case arq::Event::Failed:
        ESP_LOGW(TAG, "message seq=%u dropped after all retries", seq);
        break;
    case arq::Event::BreakerTrip:
        ESP_LOGW(TAG, "link breaker tripped: consecutive messages exhausted retries");
        break;
    case arq::Event::BreakerReset:
        ESP_LOGI(TAG, "link breaker reset");
        break;
    default:
        break;
    }
}
