#include "meguminlink/led/ws2812_led_driver.hpp"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#include <cmath>

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::led";

// RMT 通道分辨率：10MHz → 1 tick = 100ns
constexpr u32 RMT_RESOLUTION_HZ = 10'000'000;

// WS2812 位时序（对齐 grid-controller 的 SM16703/WS2812 协议，800kHz 数据率）：
// bit0 = 高 300ns + 低 900ns；bit1 = 高 700ns + 低 500ns。
// 每个 bit 共 12 ticks = 1.2µs ≈ 833kHz，落在 WS2812 允许的 800kHz±容差内。
constexpr u16 TICK_HIGH_0 = 3;  // 300ns
constexpr u16 TICK_LOW_0 = 9;   // 900ns
constexpr u16 TICK_HIGH_1 = 7;  // 700ns
constexpr u16 TICK_LOW_1 = 5;   // 500ns

// 动画帧间隔：esp_timer 每 30ms 刷一帧（呼吸/闪烁的视觉粒度）
constexpr u32 FRAME_MS = 30;
// 呼吸周期：100 帧 × 30ms = 3s（正弦波 alpha = 0.5 - 0.5*cos(2π·p/N)，p ∈ [0,N)）
constexpr u32 BREATH_PERIOD_FRAMES = 100;
// 闪烁半周期：8 帧 ≈ 240ms
constexpr u32 BLINK_HALF_PERIOD_FRAMES = 8;

// RMT 内存块：S3/C3 的 SOC_RMT_MEM_WORDS_PER_CHANNEL 均为 48
constexpr usize RMT_MEM_BLOCK_SYMBOLS = 48;
constexpr usize RMT_TX_QUEUE_DEPTH = 8;
} // namespace

esp_err_t Ws2812LedDriver::init()
{
    // RMT TX 通道：10MHz 分辨率，空闲电平低（上电即灭，
    // 帧间低电平兼作 WS2812 ≥50µs 复位信号）
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = m_io;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = RMT_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS;
    tx_cfg.trans_queue_depth = RMT_TX_QUEUE_DEPTH;
    tx_cfg.flags.init_level = 0;

    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &m_tx_channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Bytes encoder：MSB first，bit0/bit1 时序如上；数据字节序 GRB
    rmt_bytes_encoder_config_t enc_cfg = {};
    enc_cfg.bit0.duration0 = TICK_HIGH_0;
    enc_cfg.bit0.level0 = 1;
    enc_cfg.bit0.duration1 = TICK_LOW_0;
    enc_cfg.bit0.level1 = 0;
    enc_cfg.bit1.duration0 = TICK_HIGH_1;
    enc_cfg.bit1.level0 = 1;
    enc_cfg.bit1.duration1 = TICK_LOW_1;
    enc_cfg.bit1.level1 = 0;
    enc_cfg.flags.msb_first = 1;

    ret = rmt_new_bytes_encoder(&enc_cfg, &m_encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_enable(m_tx_channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 动画用周期定时器：帧间隔 FRAME_MS，仅在动画效果（Blink/Breath）时运行
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = timer_cb;
    timer_args.arg = this;
    timer_args.name = "mlink_ws2812_anim";
    ret = esp_timer_create(&timer_args, &m_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始黑色帧（上电熄灭，保证 LED 明确处于灭状态）
    send_frame(0, 0, 0);

    ESP_LOGI(TAG, "WS2812 driver: GPIO%d @%lu Hz (bit0=%u/%u ns bit1=%u/%u ns)", m_io, RMT_RESOLUTION_HZ,
             TICK_HIGH_0 * 100, TICK_LOW_0 * 100, TICK_HIGH_1 * 100, TICK_LOW_1 * 100);
    return ESP_OK;
}

void Ws2812LedDriver::apply(const Ws2812LedTarget &target)
{
    // 锁内只修改共享标志；esp_timer 启停必须在锁外（esp_timer_stop 会
    // 阻塞等待正在运行的回调返回，而回调要拿同一把锁——锁内调用会死锁）
    bool need_start = false;
    bool need_stop = false;
    {
        portENTER_CRITICAL(&m_lock);
        m_target = target;
        if (target.effect == Ws2812LedEffect::Blink || target.effect == Ws2812LedEffect::Breath)
        {
            if (!m_anim_running)
            {
                m_anim_running = true;
                m_phase = 0;
                need_start = true;
            }
        }
        else
        {
            if (m_anim_running)
            {
                m_anim_running = false;
                need_stop = true;
            }
        }
        portEXIT_CRITICAL(&m_lock);
    }

    if (need_stop)
    {
        esp_timer_stop(m_timer);
    }
    if (need_start)
    {
        esp_timer_start_periodic(m_timer, FRAME_MS * 1000u);
    }

    // 静态效果立即刷帧；动画效果也立即刷一帧（不必等定时器第一拍），
    // 之后由 timer_cb 持续驱动
    if (target.effect == Ws2812LedEffect::On)
    {
        send_frame(target.r, target.g, target.b);
    }
    else if (target.effect == Ws2812LedEffect::Off)
    {
        send_frame(0, 0, 0);
    }
    else
    {
        u8 r, g, b;
        frame_color(r, g, b);
        send_frame(r, g, b);
    }
}

void Ws2812LedDriver::timer_cb(void *arg)
{
    auto *self = static_cast<Ws2812LedDriver *>(arg);

    bool anim_running;
    portENTER_CRITICAL(&self->m_lock);
    self->m_phase += 1; // 相位先递增再计算（apply 启动时已重置为 0）
    anim_running = self->m_anim_running;
    portEXIT_CRITICAL(&self->m_lock);

    if (!anim_running)
    {
        return; // apply 已切到静态效果（stop 等待本回调返回），本帧丢弃
    }

    u8 r, g, b;
    self->frame_color(r, g, b);
    self->send_frame(r, g, b);
}

void Ws2812LedDriver::frame_color(u8 &r, u8 &g, u8 &b)
{
    Ws2812LedEffect effect;
    u8 br, bg, bb;
    u32 phase;
    portENTER_CRITICAL(&m_lock);
    effect = m_target.effect;
    br = m_target.r;
    bg = m_target.g;
    bb = m_target.b;
    phase = m_phase;
    portEXIT_CRITICAL(&m_lock);

    switch (effect)
    {
    case Ws2812LedEffect::Breath: {
        // 正弦波呼吸：alpha = 0.5 - 0.5·cos(2π·p/N)，p ∈ [0,N)，0→1→0 平滑
        const u32 p = phase % BREATH_PERIOD_FRAMES;
        const f32 alpha = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * static_cast<f32>(p) /
                                                static_cast<f32>(BREATH_PERIOD_FRAMES));
        r = static_cast<u8>(static_cast<f32>(br) * alpha);
        g = static_cast<u8>(static_cast<f32>(bg) * alpha);
        b = static_cast<u8>(static_cast<f32>(bb) * alpha);
        break;
    }
    case Ws2812LedEffect::Blink: {
        const bool on = (phase / BLINK_HALF_PERIOD_FRAMES) % 2 == 0;
        r = on ? br : 0;
        g = on ? bg : 0;
        b = on ? bb : 0;
        break;
    }
    case Ws2812LedEffect::On:
    case Ws2812LedEffect::Off:
    default:
        r = br;
        g = bg;
        b = bb;
        break;
    }
}

void Ws2812LedDriver::send_frame(u8 r, u8 g, u8 b)
{
    // WS2812 字节序为 GRB
    u8 data[3] = {g, r, b};

    rmt_transmit_config_t cfg = {};
    cfg.flags.queue_nonblocking = true; // 定时器回调上下文：队列满时丢弃本帧而非阻塞

    const esp_err_t ret = rmt_transmit(m_tx_channel, m_encoder, data, sizeof(data), &cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(ret));
    }
}
