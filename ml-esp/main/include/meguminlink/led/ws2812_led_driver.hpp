#pragma once
#ifndef __MEGUMINLINK_WS2812_LED_DRIVER_HPP__
#define __MEGUMINLINK_WS2812_LED_DRIVER_HPP__

#include "driver/rmt_types.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "meguminlink/common.hpp"
#include "sdkconfig.h"

namespace mlink
{
/** 单颗 WS2812 状态灯的目标效果 */
enum class Ws2812LedEffect
{
    Off,    // 灭
    On,     // 常亮
    Blink,  // 周期闪烁（半周期 240ms）
    Breath, // 呼吸（正弦波亮度渐变，周期 3s）
};

/** 单颗 WS2812 状态灯的目标状态：颜色（RGB888）+ 效果 */
struct Ws2812LedTarget
{
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    Ws2812LedEffect effect = Ws2812LedEffect::Off;
};

/**
 * 单颗 WS2812 状态灯驱动（MEGUMINLINK_LED_DRIVER_WS2812，RMT）。
 *
 * 协议（对齐 grid-controller 的 WS2812/SM16703 时序）：
 *   - 800kHz 数据率，每 bit = 高电平时长区分 0/1；
 *   - bit0 = 300ns 高 + 900ns 低；bit1 = 700ns 高 + 500ns 低；
 *   - 24 bit / 灯，MSB first，字节序 GRB；
 *   - 帧间低电平 ≥50µs（复位信号）。
 * RMT 通道分辨率 10MHz（100ns/tick），S3/C3 均支持。
 *
 * 效果实现：
 *   - Off/On：立即传输一次静态帧并停止动画定时器；
 *   - Blink：周期开关（半周期 = BLINK_HALF_PERIOD_FRAMES × 30ms）；
 *   - Breath：正弦波亮度呼吸（周期 = BREATH_PERIOD_FRAMES × 30ms）。
 * 动画由周期 esp_timer 回调（运行在 esp_timer 任务上下文）驱动，
 * 每帧经 rmt_transmit 异步入队（帧仅 24 symbol，队列深度足够）。
 * 与 GPIO 驱动一致，共享状态用 portMUX 临界区保护。
 */
class Ws2812LedDriver
{
  public:
    esp_err_t init();
    void apply(const Ws2812LedTarget &target);

  private:
    /** 定时器回调（esp_timer 任务上下文）：按当前效果刷一帧 */
    static void timer_cb(void *arg);

    /** 送一帧静止颜色（写入 RMT 队列，异步完成） */
    void send_frame(u8 r, u8 g, u8 b);

    /**
     * 按当前效果计算本帧颜色（Breath/Blink 依赖内部相位计数器）。
     * 无动画时返回静止色。
     */
    void frame_color(u8 &r, u8 &g, u8 &b);

    gpio_num_t m_io = static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_WS2812_GPIO);
    rmt_channel_handle_t m_tx_channel = nullptr;
    rmt_encoder_handle_t m_encoder = nullptr;
    esp_timer_handle_t m_timer = nullptr;

    Ws2812LedTarget m_target{};  /**< 当前目标状态（apply 写入，定时器读取） */
    bool m_anim_running = false; /**< 动画定时器当前是否运行 */
    u32 m_phase = 0;             /**< 动画相位计数（30ms 一帧递增） */
    portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace mlink

#endif
