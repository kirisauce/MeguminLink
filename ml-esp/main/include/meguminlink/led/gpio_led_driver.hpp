#pragma once
#ifndef __MEGUMINLINK_GPIO_LED_DRIVER_HPP__
#define __MEGUMINLINK_GPIO_LED_DRIVER_HPP__

#include "driver/gpio.h" // IWYU pragma: keep
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "sdkconfig.h"         // IWYU pragma: keep

#include <array>

namespace mlink
{
/** 单盏 LED 的目标效果（驱动与控制器共用的效果约定） */
enum class LedEffect
{
    Off,   // 灭
    On,    // 常亮
    Blink, // 周期闪烁（翻转半周期由 Kconfig 配置）
};

/** 两盏 LED 的目标效果：[0]=LED1（模式灯） [1]=LED2（配对/连接灯） */
using LedEffects = std::array<LedEffect, 2>;

/**
 * GPIO 直驱双 LED 驱动（MEGUMINLINK_LED_DRIVER_GPIO）。
 *
 * 硬件要求：LED 经限流电阻接 GPIO；默认低电平点亮（阴极接 GPIO、
 * 阳极接 VCC），也支持高电平点亮（Kconfig ACTIVE_LOW=n）。
 * 闪烁由周期 esp_timer 回调翻转实现，不占用任务；共享状态用 portMUX
 * 临界区保护（apply 与定时器回调分属不同任务上下文）。
 */
class GpioLedDriver
{
  public:
    esp_err_t init();
    void apply(const LedEffects &effects);

  private:
    static void timer_cb(void *arg);

    /** 按“是否点亮”写入实际 GPIO 电平（考虑有效电平极性） */
    void write_level(int index, bool lit);

    gpio_num_t m_io[2] = {static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_LED1_IO),
                          static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_LED2_IO)};
    bool m_active_low; /**< 低电平点亮 */
    esp_timer_handle_t m_timer = nullptr;
    LedEffects m_effects{}; /**< 当前目标效果（apply 写入，定时器读取） */
    bool m_level[2] = {};   /**< 当前实际 GPIO 电平（Blink 相位） */
    portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace mlink

#endif
