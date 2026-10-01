#pragma once
#ifndef __MEGUMINLINK_LED_DRIVER_HPP__
#define __MEGUMINLINK_LED_DRIVER_HPP__

#include "meguminlink/led/led_types.hpp"
#include "sdkconfig.h"

// 驱动选型：编译期绑定具体驱动实现（无虚函数、无堆分配）。未来新增驱动（如 LEDC
// PWM）：在 Kconfig choice 中加选项，并在此追加一个 #elif 分支切换 using 即可。
#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
#include "meguminlink/led/gpio_led_driver.hpp"
#elif CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
#include "meguminlink/led/ws2812_led_driver.hpp"
#endif

namespace mlink
{
#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
/** 当前 Kconfig 选中的 LED 驱动（由使用方静态持有：Application 与更新模式各一份） */
using LedDriver = GpioLedDriver;
#elif CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
using LedDriver = Ws2812LedDriver;
#else
/**
 * LED 模块关闭（MEGUMINLINK_LED_DRIVER_NONE）时回退到空驱动：调用方照常上报
 * 场景，不必自己判断"LED 是否编进了固件"（见 NullLedDriver）。
 */
using LedDriver = NullLedDriver;
#endif

/** 选型落地前的编译期把关：接口写歪在此报错，而不是等到某个调用点冒出一堆模板实例化错误 */
static_assert(LedDriverConcept<LedDriver>, "the selected LED driver does not satisfy LedDriverConcept");
} // namespace mlink

#endif
