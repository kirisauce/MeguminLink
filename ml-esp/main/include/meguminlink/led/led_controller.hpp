#pragma once
#ifndef __MEGUMINLINK_LED_CONTROLLER_HPP__
#define __MEGUMINLINK_LED_CONTROLLER_HPP__

#include "esp_err.h"
#include "meguminlink/led/led_driver.hpp"

namespace mlink
{
class Application; // 前置声明：LedController 自行读取 Application，避免与 app.hpp 循环依赖

/**
 * LED 指示灯控制器（门面）。
 *
 * 外部（Application）只负责两件事：init 时注入自身引用、主循环中周期性
 * 调用 update()。状态读取（模式/配对）与“状态 → 每盏 LED 效果”的映射全部
 * 在本类内部完成；实际驱动由编译期选型（见 led_driver.hpp）。
 */
class LedController
{
  public:
    esp_err_t init(Application &app);
    void update();

  private:
    Application *m_app = nullptr;
    LedDriver m_driver;
};

} // namespace mlink

#endif
