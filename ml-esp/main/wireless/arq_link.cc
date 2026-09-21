#include "meguminlink/wireless/arq_link.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <sdkconfig.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::arq_link";

namespace
{
// NVS 键名上限 15 字符
constexpr char KEY_ARQ_GENERATION[] = "arq_gen";
constexpr char KEY_ARQ_ENABLE[] = "arq_enable";
constexpr char KEY_ARQ_WINDOW[] = "arq_window";
constexpr char KEY_ARQ_MAX_RETX[] = "arq_max_retx";
constexpr char KEY_ARQ_BREAKER[] = "arq_breaker";
constexpr char KEY_ARQ_RTO_MIN[] = "arq_rto_min";
constexpr char KEY_ARQ_RTO_INIT[] = "arq_rto_init";
constexpr char KEY_ARQ_RTO_MAX[] = "arq_rto_max";
constexpr char KEY_ARQ_DELAYED_ACK[] = "arq_delayed_ack";
constexpr char KEY_ARQ_BREAKER_PROBE[] = "arq_probe";
} // namespace

// ---- 参数：默认值、校验、NVS 存取 ----

ArqSettings mlink::arq_default_settings()
{
    ArqSettings out = {};
    out.generation = 0;
    out.enable = 1;
    out.window = 8;
    out.max_retx = 6;
    out.breaker_failures = 3;
    out.rto_min_ms = 60;
    out.initial_rto_ms = 60;
    out.rto_max_ms = 250;
    out.delayed_ack_ms = 2;
    out.breaker_probe_ms = 1000;
    return out;
}

arq::Config mlink::arq_config_from(const ArqSettings &settings)
{
    arq::Config config;
    config.window = settings.window;
    // 窗口大于 1 必须开重排：同时在途的多条消息一旦乱序，只能靠乱序缓冲补齐，
    // 否则整窗判超窗、退化成靠重传逐条补齐
    config.reorder = (settings.window > 1);
    config.rto_min_ms = settings.rto_min_ms;
    config.initial_rto_ms = settings.initial_rto_ms;
    config.rto_max_ms = settings.rto_max_ms;
    config.max_retx = settings.max_retx;
    config.breaker_failures = settings.breaker_failures;
    config.delayed_ack_ms = settings.delayed_ack_ms;
    config.breaker_probe_ms = settings.breaker_probe_ms;
    return config; // 越界值由核心自己收敛
}

namespace
{
/**
 * 把窗口收敛到本固件编译期允许的上限（Kconfig 的 ARQ_MAX_WINDOW）。
 *
 * 主机下发的值与对端传来的值都可能是更大窗口那份固件给的；本固件按自己的上限
 * 为发送窗口与乱序缓冲分配静态存储，因此只认自己的上限——收敛后再存，上报与
 * 转发出去的也就是这个值，不会出现"声称支持 8、实际只按 1 跑"。
 */
ArqSettings clamp_window(ArqSettings settings)
{
    const u8 cap = static_cast<u8>(CONFIG_MEGUMINLINK_WIRELESS_ARQ_MAX_WINDOW);
    if (settings.window > cap)
    {
        settings.window = cap;
    }
    return settings;
}
} // namespace

bool mlink::arq_settings_valid(const ArqSettings &settings)
{
    return settings.window >= 1 && settings.window <= arq::MAX_WINDOW && settings.max_retx >= 1 &&
           settings.rto_min_ms >= 1 && settings.rto_max_ms >= settings.rto_min_ms &&
           settings.initial_rto_ms >= 1 && settings.delayed_ack_ms >= 1 && settings.breaker_probe_ms >= 1;
}

ArqSettings mlink::arq_settings_load()
{
    ArqSettings out = arq_default_settings();

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return out; // 无记录：用默认值
    }

    u32 u32_value = 0;
    u16 u16_value = 0;
    u8 u8_value = 0;

    if (nvs_get_u32(handle, KEY_ARQ_GENERATION, &u32_value) == ESP_OK)
    {
        out.generation = u32_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_ENABLE, &u8_value) == ESP_OK)
    {
        out.enable = (u8_value != 0) ? 1 : 0;
    }
    if (nvs_get_u8(handle, KEY_ARQ_WINDOW, &u8_value) == ESP_OK)
    {
        out.window = u8_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_MAX_RETX, &u8_value) == ESP_OK)
    {
        out.max_retx = u8_value;
    }
    if (nvs_get_u8(handle, KEY_ARQ_BREAKER, &u8_value) == ESP_OK)
    {
        out.breaker_failures = u8_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_MIN, &u16_value) == ESP_OK)
    {
        out.rto_min_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_INIT, &u16_value) == ESP_OK)
    {
        out.initial_rto_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_RTO_MAX, &u16_value) == ESP_OK)
    {
        out.rto_max_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_DELAYED_ACK, &u16_value) == ESP_OK)
    {
        out.delayed_ack_ms = u16_value;
    }
    if (nvs_get_u16(handle, KEY_ARQ_BREAKER_PROBE, &u16_value) == ESP_OK)
    {
        out.breaker_probe_ms = u16_value;
    }

    nvs_close(handle);

    if (!arq_settings_valid(out))
    {
        // 落盘的值越界（旧固件写过、flash 损坏）：回默认，免得带着非法参数运行
        ESP_LOGW(TAG, "stored ARQ settings invalid, falling back to defaults");
        return arq_default_settings();
    }
    return out;
}

esp_err_t mlink::arq_settings_store(const ArqSettings &settings)
{
    NvsWriteBatch batch(NVS_NAMESPACE);
    if (!batch.ready())
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 整份参数走同一个 commit：NVS 保证要么全生效、要么全不生效
    esp_err_t ret = batch.set_u32(KEY_ARQ_GENERATION, settings.generation);
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_ENABLE, settings.enable ? 1 : 0);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_WINDOW, settings.window);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_MAX_RETX, settings.max_retx);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u8(KEY_ARQ_BREAKER, settings.breaker_failures);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_MIN, settings.rto_min_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_INIT, settings.initial_rto_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_RTO_MAX, settings.rto_max_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_DELAYED_ACK, settings.delayed_ack_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ARQ_BREAKER_PROBE, settings.breaker_probe_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.commit();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to store ARQ settings: %s", esp_err_to_name(ret));
    }
    return ret;
}

// ---- 构造与配置 ----

esp_err_t ArqLink::init(const ArqSettings &settings, size_t payload_cap)
{
    if (payload_cap == 0 || payload_cap > MAX_PAYLOAD || !arq_settings_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    m_settings = clamp_window(settings);
    m_config = arq_config_from(m_settings);
    m_payload_cap = payload_cap;

    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    // 本机 MAC：参数平局时用它做确定性判断
    esp_read_mac(m_own_mac, ESP_MAC_WIFI_STA);

    reconfigure_locked();
    reset_session(0);
    return ESP_OK;
}

void ArqLink::reconfigure_locked()
{
    m_config = arq_config_from(m_settings);
    // 参数变了就必须重建两端状态：窗口、重传预算与退避全都按新参数算。累计计数
    // 一并归零是刻意的——参数变更即一次新的测量起点，做 A/B 对照时正好干净。
    m_sender.init(m_config, m_tx_storage, m_payload_cap, arq::MAX_WINDOW);
    m_receiver.init(m_config);
    m_rx_reorder.init(m_rx_storage, m_payload_cap, arq::MAX_WINDOW);
    m_sender.set_callbacks(this, &ArqLink::core_transmit, &ArqLink::core_event);
    m_receiver.set_callbacks(this, &ArqLink::core_event);
}

esp_err_t ArqLink::apply_settings(const ArqSettings &settings, u32 now_ms)
{
    if (!arq_settings_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    m_settings = clamp_window(settings);
    reconfigure_locked();
    m_settings_dirty = true; // 等主循环落盘（NVS 写入不放这里）
    unlock();

    // 换新 epoch 重置会话：对端据此重新对齐序号，并顺手采纳这份新参数
    reset_session(now_ms);
    return ESP_OK;
}

ArqSettings ArqLink::settings() const
{
    lock();
    const ArqSettings out = m_settings;
    unlock();
    return out;
}

bool ArqLink::take_settings_dirty()
{
    lock();
    const bool dirty = m_settings_dirty;
    m_settings_dirty = false;
    unlock();
    return dirty;
}

bool ArqLink::should_adopt_settings_locked(const ArqSettings &peer) const
{
    const i32 diff = static_cast<i32>(peer.generation - m_settings.generation);
    if (diff > 0)
    {
        return true;
    }
    if (diff < 0)
    {
        return false;
    }

    // 代数相同（典型情形是两块板一起上电）：用 MAC 大小做确定性平局。两端比的是
    // 同一个不等式，因此算出的赢家一致——MAC 小的一方那份参数会被两端共同采用，
    // 不会出现"互相采纳、结果仍然不一致"。
    if (!m_peer_known)
    {
        return false;
    }
    return std::memcmp(m_own_mac, m_peer_mac, sizeof(m_own_mac)) > 0;
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
                m_callbacks.send_reset(m_callbacks.ctx, action.epoch, action.settings, action.reset_echo, action.ack,
                                       action.sack, action.window);
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
    // 运行时关闭时不需要序号对齐：没有窗口、没有重传，也就没有"基准"这回事
    const bool value = m_established || !m_settings.enable;
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
    m_rx_reorder.reset();
    m_established = false;
    m_ack_scheduled = false;
    m_replay_pending = false;
    m_reset_reply_pending = false;
    m_action_count = 0;
    m_peer_epoch_known = false;
    std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));
    m_failed_head = 0;
    m_failed_count = 0;

    // 换一个 epoch：对端据此把"本端重启后的新会话"与"旧会话的迟到帧"分开
    m_epoch = esp_random() | 1u;
    m_next_reset_tx_ms = now_ms;
    unlock();
}

// ---- 接收路径（ESP-NOW 回调上下文：只更新状态与待发标记）----

void ArqLink::on_ack(u16 ack, u16 sack, u8 window, u32 now_ms)
{
    lock();
    if (m_settings.enable)
    {
        m_sender.on_ack(ack, sack, window, now_ms);
    }
    unlock();
}

void ArqLink::on_message(u16 seq, bool gap_before, const u8 *payload, u16 len, u32 now_ms)
{
    lock();

    if (!m_settings.enable)
    {
        // 运行时关闭：不判重、不排序，收到就交付（跳过核心）
        if (m_callbacks.deliver != nullptr)
        {
            u8 peer_mac[6] = {};
            if (m_peer_known)
            {
                std::memcpy(peer_mac, m_peer_mac, sizeof(peer_mac));
            }
            m_callbacks.deliver(m_callbacks.ctx, m_peer_known ? peer_mac : nullptr, seq, payload, len);
        }
        unlock();
        return;
    }

    const arq::RxResult result = m_receiver.on_frame(seq, gap_before, now_ms);
    if (result == arq::RxResult::Delivered)
    {
        // commit 返回本次连续交付的最后一帧：乱序缓冲补齐时区间会不止一帧
        const u16 last = m_receiver.commit(seq, now_ms);
        deliver_range_locked(seq, payload, len, last);
    }
    else if (result == arq::RxResult::Buffered)
    {
        // 判为窗口内的乱序帧：先把载荷收进乱序缓冲，等 commit 的区间把它补出来
        m_rx_reorder.store(seq, payload, len);
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

void ArqLink::deliver_range_locked(u16 first, const u8 *first_payload, u16 first_len, u16 last)
{
    u8 peer_mac[6] = {};
    const bool peer_known = m_peer_known;
    if (peer_known)
    {
        std::memcpy(peer_mac, m_peer_mac, sizeof(peer_mac));
    }

    for (u16 seq = first;; ++seq)
    {
        const u8 *payload = first_payload;
        u16 len = first_len;
        if (seq != first)
        {
            payload = m_rx_reorder.get(seq, &len);
            if (payload == nullptr)
            {
                // 区间内没有载荷：对端声明放弃的空洞，跳过不交付
                if (seq == last)
                {
                    break;
                }
                continue;
            }
        }

        if (m_callbacks.deliver != nullptr)
        {
            m_callbacks.deliver(m_callbacks.ctx, peer_known ? peer_mac : nullptr, seq, payload, len);
        }

        if (seq != first)
        {
            m_rx_reorder.release(seq);
        }
        if (seq == last)
        {
            break;
        }
    }
}

void ArqLink::on_reset(u32 peer_epoch, const ArqSettings &peer_settings, bool echo, u32 now_ms)
{
    lock();

    bool reply = false;
    if (!m_peer_epoch_known || m_peer_epoch != peer_epoch)
    {
        // 对端开了一个新会话（上电、链路重建或它刚被下发了新参数）：本端把状态
        // 拉到同一序号基准
        m_peer_epoch = peer_epoch;
        m_peer_epoch_known = true;
        m_sender.reset();
        m_receiver.reset(0);
        m_ack_scheduled = false;
        m_replay_pending = false;
        std::memset(m_replay_map_valid, 0, sizeof(m_replay_map_valid));
        m_established = true;
        reply = true;

        // 参数只在对端换了 epoch（即它确实重启过一次会话）时才评估采纳，回显帧
        // 因此不会来回改参数；采纳规则见 should_adopt_settings_locked
        if (arq_settings_valid(peer_settings) && should_adopt_settings_locked(peer_settings))
        {
            m_settings = clamp_window(peer_settings);
            reconfigure_locked();
            m_settings_dirty = true; // 落盘交给主循环
            ESP_LOGI(TAG, "adopted peer ARQ settings (generation %u)", static_cast<unsigned>(peer_settings.generation));
        }
        ESP_LOGI(TAG, "peer session epoch 0x%08X accepted, sequence space realigned", peer_epoch);
    }
    else if (!m_established)
    {
        // 同一会话但本端还没对齐：对端仍在重发请求，对齐并回一个
        m_established = true;
        reply = true;
    }
    else if (!echo)
    {
        // 会话已建立却又收到同 epoch 的请求：本端上一次回显没有送达，再回一次
        reply = true;
    }
    // 会话已建立且收到的是回显：对端只是在确认它收到了本端请求，回显到此为止。
    // 少了这一条，两端会把对方的回显当成新的请求，互相回显下去永不停止。

    // 回显只在这里登记，真正的发送在 poll 里（这里是 ESP-NOW 接收回调的上下文）
    if (reply)
    {
        m_reset_reply_pending = true;
        if (static_cast<i32>(now_ms - m_reset_reply_due_ms) >= 0)
        {
            m_reset_reply_due_ms = now_ms + RESET_REPLY_MIN_MS;
        }
    }

    unlock();
}

// ---- 任务上下文 ----

void ArqLink::poll(u32 now_ms)
{
    Action actions[MAX_ACTIONS];

    lock();

    // 1) 会话未对齐：周期性发重置请求（echo=false），直到收到对端的 epoch
    if (!m_established && m_peer_known && static_cast<i32>(now_ms - m_next_reset_tx_ms) >= 0)
    {
        queue_locked(make_reset_action_locked(false));
        m_next_reset_tx_ms = now_ms + RESET_INTERVAL_MS;
    }

    // 1b) 对端请求的回显（echo=true，限速）
    if (m_reset_reply_pending && static_cast<i32>(now_ms - m_reset_reply_due_ms) >= 0)
    {
        queue_locked(make_reset_action_locked(true));
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

ArqLink::Action ArqLink::make_reset_action_locked(bool echo) const
{
    Action action = {};
    action.kind = Action::Kind::Reset;
    action.reset_echo = echo;
    action.epoch = m_epoch;
    action.ack = m_receiver.ack();
    action.sack = m_receiver.sack_bitmap();
    action.window = m_receiver.window();
    action.settings = m_settings;
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
    if (m_peer_known && !m_settings.enable)
    {
        // 运行时关闭：绕过核心，发完即忘，等价于尽力而为。帧格式仍是带扩展头的
        // 新格式，所以 A/B 对照只差"ARQ 有没有生效"这一件事。
        if (payload == nullptr || len == 0 || len > m_payload_cap)
        {
            result = Submit::Invalid;
        }
        else
        {
            std::memcpy(m_tx_storage, payload, len);
            Action action = {};
            action.kind = Action::Kind::Message;
            action.seq = 0;
            action.len = len;
            action.ack = m_receiver.ack();
            action.sack = m_receiver.sack_bitmap();
            action.window = m_receiver.window();
            queue_locked(action);
            if (seq_out != nullptr)
            {
                *seq_out = 0;
            }
            result = Submit::Ok;
        }
    }
    else if (m_peer_known && m_established)
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

void ArqLink::record_failed_locked(u16 seq)
{
    if (m_failed_count == arq::MAX_WINDOW)
    {
        // 队列满：挤掉最老的一条。上层没跟上时保最新更有用——旧的那条早已经
        // 被自己的响应超时销账了。
        m_failed_head = static_cast<u8>((m_failed_head + 1) % arq::MAX_WINDOW);
        m_failed_count--;
    }
    const u8 tail = static_cast<u8>((m_failed_head + m_failed_count) % arq::MAX_WINDOW);
    m_failed_seqs[tail] = seq;
    m_failed_count++;
}

bool ArqLink::take_failed(u16 *seq_out)
{
    lock();
    if (m_failed_count == 0)
    {
        unlock();
        return false;
    }

    if (seq_out != nullptr)
    {
        *seq_out = m_failed_seqs[m_failed_head];
    }
    m_failed_head = static_cast<u8>((m_failed_head + 1) % arq::MAX_WINDOW);
    m_failed_count--;
    unlock();
    return true;
}

bool ArqLink::can_submit() const
{
    lock();

    bool value = false;
    if (!m_settings.enable)
    {
        // 绕过核心：没有窗口，配对在就能发
        value = m_peer_known;
    }
    else if (m_peer_known && m_established)
    {
        const arq::SenderSnapshot snap = m_sender.snapshot();
        value = !snap.breaker_open && snap.in_flight < m_sender.window();
    }

    unlock();
    return value;
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
        // 上抛给调用方销账：这条消息不会再有响应，在途条目要尽快释放
        self->record_failed_locked(seq);
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
