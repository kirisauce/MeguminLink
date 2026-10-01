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
constexpr u16 TICK_HIGH_0 = 3; // 300ns
constexpr u16 TICK_LOW_0 = 9;  // 900ns
constexpr u16 TICK_HIGH_1 = 7; // 700ns
constexpr u16 TICK_LOW_1 = 5;  // 500ns

// 动画帧间隔：esp_timer 每 30ms 刷一帧（呼吸/闪烁/渐变的视觉粒度）
constexpr u32 FRAME_MS = 30;
// 呼吸周期：100 帧 × 30ms = 3s（正弦波 alpha = 0.5 - 0.5*cos(2π·p/N)，p ∈ [0,N)）
constexpr u32 BREATH_PERIOD_FRAMES = 100;
// 闪烁半周期：8 帧 ≈ 240ms
constexpr u32 BLINK_HALF_PERIOD_FRAMES = 8;
// 空闲渐变周期：三段等分，每段 100 帧 × 30ms = 3s，一轮 9s
constexpr u32 GRADIENT_CYCLE_FRAMES = 300;

constexpr f32 PI = 3.14159265f;

// ---- 场景配色 ----

constexpr LedColor LOCAL_COLOR{0, 255, 0};    // 本地调试：绿色
constexpr LedColor WIRELESS_COLOR{0, 0, 255}; // 无线调试：蓝色（配对中呼吸）
constexpr LedColor VERIFY_COLOR{255, 192, 0}; // 校验镜像：琥珀色
constexpr LedColor DONE_COLOR{0, 255, 0};     // 升级成功：绿色闪烁
constexpr LedColor FAILED_COLOR{255, 0, 0};   // 升级失败：红色闪烁

// 接收进度配色：绿灯分量跟着进度从 64 涨到 255（蓝→青），一眼看出还剩多少
constexpr u8 RECEIVE_GREEN_FROM = 64;
constexpr u8 RECEIVE_GREEN_TO = 255;
constexpr u8 RECEIVE_BLUE = 255;

// 空闲渐变调色板：红 → 橙 → 粉（末段由粉插值回红）
constexpr LedColor PALETTE[] = {
    {255, 0, 0},
    {255, 128, 0},
    {255, 64, 160},
};
constexpr u32 PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);
constexpr u32 SEGMENT_FRAMES = GRADIENT_CYCLE_FRAMES / PALETTE_SIZE;

static_assert(GRADIENT_CYCLE_FRAMES % PALETTE_SIZE == 0,
              "gradient cycle must split evenly into one segment per palette entry");

// RMT 内存块：S3/C3 的 SOC_RMT_MEM_WORDS_PER_CHANNEL 均为 48
constexpr usize RMT_MEM_BLOCK_SYMBOLS = 48;
constexpr usize RMT_TX_QUEUE_DEPTH = 8;

/** 整数线性插值：step ∈ [0, span)，结果只落在 from 与 to 之间 */
u8 lerp(u8 from, u8 to, u32 step, u32 span)
{
    const i32 delta = static_cast<i32>(to) - static_cast<i32>(from);
    return static_cast<u8>(static_cast<i32>(from) + delta * static_cast<i32>(step) / static_cast<i32>(span));
}

/** 颜色整体按 alpha（0-1）缩放，用于呼吸的亮度调制 */
LedColor scale(const LedColor &color, f32 alpha)
{
    return LedColor{static_cast<u8>(static_cast<f32>(color.r) * alpha),
                    static_cast<u8>(static_cast<f32>(color.g) * alpha),
                    static_cast<u8>(static_cast<f32>(color.b) * alpha)};
}

/** 闪烁当前应处于亮半周期还是暗半周期 */
bool blink_lit(u32 phase)
{
    return (phase / BLINK_HALF_PERIOD_FRAMES) % 2 == 0;
}

/** 空闲渐变的第 phase 帧颜色：调色板线性插值循环（三段都在红色通道拉满） */
LedColor gradient_color(u32 phase)
{
    const u32 position = phase % GRADIENT_CYCLE_FRAMES;
    const u32 segment = position / SEGMENT_FRAMES;
    const u32 step = position % SEGMENT_FRAMES;
    const LedColor &from = PALETTE[segment];
    const LedColor &to = PALETTE[(segment + 1) % PALETTE_SIZE];

    return LedColor{lerp(from.r, to.r, step, SEGMENT_FRAMES), lerp(from.g, to.g, step, SEGMENT_FRAMES),
                    lerp(from.b, to.b, step, SEGMENT_FRAMES)};
}
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

    // 动画用周期定时器：帧间隔 FRAME_MS，仅在动画场景（呼吸/闪烁/渐变）时运行
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

void Ws2812LedDriver::apply_impl(const LedRequest &request)
{
    // 锁内只修改共享状态；esp_timer 启停必须在锁外（esp_timer_stop 会阻塞等待
    // 正在运行的回调返回，而回调要拿同一把锁——锁内调用会死锁）
    bool need_start = false;
    bool need_stop = false;
    {
        portENTER_CRITICAL(&m_lock);
        m_scene = request.scene;
        m_progress = request.progress;
        if (scene_animated(request.scene))
        {
            if (!m_anim_running)
            {
                m_anim_running = true;
                m_phase = 0; // 新动画从头开始；重复下发同一场景不重置相位
                need_start = true;
            }
        }
        else if (m_anim_running)
        {
            m_anim_running = false;
            need_stop = true;
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

    // 静态场景就这一帧；动画场景也立即刷一帧（不必等定时器第一拍），
    // 之后由 timer_cb 持续驱动
    u8 r, g, b;
    frame_color(r, g, b);
    send_frame(r, g, b);
}

bool Ws2812LedDriver::scene_animated(LedScene scene)
{
    switch (scene)
    {
    case LedScene::Pairing:      // 呼吸
    case LedScene::UpdateIdle:   // 渐变循环
    case LedScene::UpdateDone:   // 闪烁
    case LedScene::UpdateFailed: // 闪烁
        return true;
    default:
        return false;
    }
}

void Ws2812LedDriver::timer_cb(void *arg)
{
    auto *self = static_cast<Ws2812LedDriver *>(arg);

    bool anim_running;
    portENTER_CRITICAL(&self->m_lock);
    self->m_phase += 1; // 相位先递增再计算（apply_impl 启动动画时已重置为 0）
    anim_running = self->m_anim_running;
    portEXIT_CRITICAL(&self->m_lock);

    if (!anim_running)
    {
        return; // apply_impl 已切到静态场景（stop 等待本回调返回），本帧丢弃
    }

    u8 r, g, b;
    self->frame_color(r, g, b);
    self->send_frame(r, g, b);
}

void Ws2812LedDriver::frame_color(u8 &r, u8 &g, u8 &b)
{
    LedScene scene;
    u8 progress;
    u32 phase;
    portENTER_CRITICAL(&m_lock);
    scene = m_scene;
    progress = m_progress;
    phase = m_phase;
    portEXIT_CRITICAL(&m_lock);

    LedColor color;
    switch (scene)
    {
    case LedScene::Pairing: {
        // 正弦波呼吸：alpha = 0.5 - 0.5·cos(2π·p/N)，p ∈ [0,N)，0→1→0 平滑
        const u32 p = phase % BREATH_PERIOD_FRAMES;
        const f32 alpha =
            0.5f - 0.5f * std::cos(2.0f * PI * static_cast<f32>(p) / static_cast<f32>(BREATH_PERIOD_FRAMES));
        color = scale(WIRELESS_COLOR, alpha);
        break;
    }
    case LedScene::UpdateIdle:
        color = gradient_color(phase);
        break;
    case LedScene::UpdateReceiving:
        color = LedColor{0, lerp(RECEIVE_GREEN_FROM, RECEIVE_GREEN_TO, progress, 100), RECEIVE_BLUE};
        break;
    case LedScene::UpdateVerifying:
        color = VERIFY_COLOR;
        break;
    case LedScene::UpdateDone:
        color = blink_lit(phase) ? DONE_COLOR : LedColor{};
        break;
    case LedScene::UpdateFailed:
        color = blink_lit(phase) ? FAILED_COLOR : LedColor{};
        break;
    case LedScene::Wireless:
        color = WIRELESS_COLOR;
        break;
    case LedScene::Local:
    default:
        color = LOCAL_COLOR;
        break;
    }

    r = color.r;
    g = color.g;
    b = color.b;
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
