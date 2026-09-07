#pragma once
#ifndef __MEGUMINLINK_LED_DRIVER_HPP__
#define __MEGUMINLINK_LED_DRIVER_HPP__

#include "sdkconfig.h"

// 驱动选型：编译期绑定具体驱动实现（无虚函数、无堆分配）。
// 未来新增驱动（如 LEDC PWM）：在 Kconfig choice 中加选项，并在此追加
// 一个 #elif 分支切换 using 即可。
#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
#include "meguminlink/led/gpio_led_driver.hpp"
#elif CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
#include "meguminlink/led/ws2812_led_driver.hpp"
#endif

namespace mlink
{
#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
/** 当前 Kconfig 选中的 LED 驱动（LedController 静态持有） */
using LedDriver = GpioLedDriver;
#elif CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
using LedDriver = Ws2812LedDriver;
#endif
} // namespace mlink

#endif
