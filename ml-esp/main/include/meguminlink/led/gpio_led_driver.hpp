#pragma once
#ifndef __MEGUMINLINK_GPIO_LED_DRIVER_HPP__
#define __MEGUMINLINK_GPIO_LED_DRIVER_HPP__

#include "driver/gpio.h" // IWYU pragma: keep
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "meguminlink/led/led_types.hpp"
#include "sdkconfig.h" // IWYU pragma: keep

#include <array>

namespace mlink
{
/**
 * GPIO 直驱双 LED 驱动（MEGUMINLINK_LED_DRIVER_GPIO）。
 *
 * 硬件要求：LED 经限流电阻接 GPIO；默认低电平点亮（阴极接 GPIO、
 * 阳极接 VCC），也支持高电平点亮（Kconfig ACTIVE_LOW=n）。
 *
 * 场景渲染：GPIO 只有亮/灭两个状态，场景只能靠常亮与闪烁表达——
 * LED1 模式灯（Local 常亮）、LED2 配对/连接灯（Wireless 常亮、配对中闪烁）。
 * 更新模式不点亮任何灯：该模式属于设备自维护通道，状态指示交给支持颜色的
 * 驱动（WS2812 板），本驱动没有可用的表现手段。
 *
 * 闪烁由周期 esp_timer 回调翻转实现，不占用任务；共享状态用 portMUX
 * 临界区保护（场景下发与定时器回调分属不同任务上下文）。
 */
class GpioLedDriver : public LedDriverBase<GpioLedDriver>
{
  public:
    esp_err_t init();

    /** 场景渲染（由基类 apply 调用，勿直接调用：绕过它会丢掉下发去重） */
    void apply_impl(const LedRequest &request);

  private:
    /** 单盏 GPIO 灯的效果（GPIO 没有颜色，只能用亮/灭/闪表达场景） */
    enum class Effect : u8
    {
        Off,   // 灭
        On,    // 常亮
        Blink, // 周期闪烁（翻转半周期由 Kconfig 配置）
    };

    /** 两盏 LED 的目标效果：[0]=LED1（模式灯） [1]=LED2（配对/连接灯） */
    using Effects = std::array<Effect, 2>;

    /** 场景 → 双灯效果映射（改灯语义只需动这里） */
    static Effects scene_effects(LedScene scene);

    static void timer_cb(void *arg);

    /** 按“是否点亮”写入实际 GPIO 电平（考虑有效电平极性） */
    void write_level(int index, bool lit);

    gpio_num_t m_io[2] = {static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_LED1_IO),
                          static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_LED2_IO)};
    bool m_active_low; /**< 低电平点亮 */
    esp_timer_handle_t m_timer = nullptr;
    Effects m_effects{};  /**< 当前目标效果（场景下发写入，定时器读取） */
    bool m_level[2] = {}; /**< 当前实际 GPIO 电平（Blink 相位） */
    portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace mlink

#endif
