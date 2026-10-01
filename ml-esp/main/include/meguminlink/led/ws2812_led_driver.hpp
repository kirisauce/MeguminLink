#pragma once
#ifndef __MEGUMINLINK_WS2812_LED_DRIVER_HPP__
#define __MEGUMINLINK_WS2812_LED_DRIVER_HPP__

#include "driver/rmt_types.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "meguminlink/led/led_types.hpp"
#include "sdkconfig.h"

namespace mlink
{
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
 * 场景渲染（本驱动独自决定表现，上层只报场景）：
 *   - Local 绿常亮 / Wireless 蓝常亮 / Pairing 蓝呼吸；
 *   - 更新模式：空闲 9s 调色板渐变循环、接收中蓝→青按进度、校验中琥珀常亮、
 *     成功绿闪、失败红闪。
 * 呼吸、闪烁与渐变由周期 esp_timer 回调按 30ms 一帧推进；静态场景只在场景变化时
 * 发一帧，不把定时器常驻挂着。与 GPIO 驱动一致，共享状态用 portMUX 临界区保护。
 */
class Ws2812LedDriver : public LedDriverBase<Ws2812LedDriver>
{
  public:
    esp_err_t init();

    /** 场景渲染（由基类 apply 调用，勿直接调用：绕过它会丢掉下发去重） */
    void apply_impl(const LedRequest &request);

  private:
    /** 定时器回调（esp_timer 任务上下文）：按当前场景刷一帧 */
    static void timer_cb(void *arg);

    /** 该场景是否需要持续动画（呼吸/闪烁/渐变）；静态场景只在场景变化时刷一帧 */
    static bool scene_animated(LedScene scene);

    /** 按当前场景、进度与相位算本帧颜色（相位由动画定时器推进） */
    void frame_color(u8 &r, u8 &g, u8 &b);

    /** 送一帧颜色（写入 RMT 队列，异步完成） */
    void send_frame(u8 r, u8 g, u8 b);

    gpio_num_t m_io = static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_WS2812_GPIO);
    rmt_channel_handle_t m_tx_channel = nullptr;
    rmt_encoder_handle_t m_encoder = nullptr;
    esp_timer_handle_t m_timer = nullptr;

    LedScene m_scene = LedScene::Local; /**< 当前场景（场景下发写入，定时器读取） */
    u8 m_progress = 0;                  /**< 接收进度（同上） */
    bool m_anim_running = false;        /**< 动画定时器当前是否运行 */
    u32 m_phase = 0;                    /**< 动画相位计数（30ms 一帧递增） */
    portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace mlink

#endif
