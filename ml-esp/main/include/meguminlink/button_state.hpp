#pragma once
#ifndef __MEGUMINLINK_BUTTON_STATE_HPP__
#define __MEGUMINLINK_BUTTON_STATE_HPP__

#include "driver/gpio.h" // IWYU pragma: keep
#include "esp_err.h"
#include "esp_timer.h"
#include "meguminlink/common.hpp"

namespace mlink
{
/**
 * 模式切换按键状态机：识别防抖后的单次按下 / 双击 / 长按事件。
 *
 * 职责边界：
 * - 本类只负责按键事件识别（GPIO 边沿 → 事件标志），不关心事件含义；
 *   事件由调用方消费并执行具体动作。
 * - ISR 侧只做轻量操作（置标志、启停定时器）。长按判定：按下启动一次性
 *   定时器，松开立即取消——只有"按下后持续未松开"定时器才会到期，到期即
 *   长按（回调运行在 esp_timer 任务上下文）。周期性点击每次松开都会取消
 *   定时器，不会被误判为长按。
 *
 * 使用方法：init() 注册 GPIO 与 ISR 后，由 Application 每轮轮询
 * consume_double_click() / consume_button_press() / consume_long_press()。
 */
class ButtonState
{
  public:
    /** 配置 GPIO（内部上拉、任意边沿触发）并注册 ISR 与长按定时器 */
    esp_err_t init(gpio_num_t gpio);

    /** 消费并清除事件标志（ISR 置位、调用方清除），返回是否有事件 */
    bool consume_double_click();
    bool consume_button_press();
    bool consume_long_press();

    /**
     * 抑制长按识别：置位时按下只产生单次按下事件，不再启动长按定时器。
     * 供上层模式约束使用（例如配对模式下任意按下即退出，长按无意义）。
     */
    void set_long_press_suppressed(bool suppressed);

  private:
    // 防抖窗口：忽略间隔小于该值的按下边沿抖动（机械抖动通常 <5ms，20ms 足够）。
    // 必须远小于双击窗口（默认 400ms），否则真实双击会被防抖吞掉。
    // 仅作用于按下边沿；松开边沿不防抖（极短单击也必须取消长按定时器）。
    static constexpr u64 DEBOUNCE_US = 20 * 1000ULL;

    gpio_num_t m_gpio = GPIO_NUM_NC;
    esp_timer_handle_t m_long_press_timer = nullptr;

    // ISR 写、调用方读：bool 单字节对齐访问在 32 位目标上原子，volatile 防缓存
    volatile u64 m_last_edge_us = 0;   // 最近一次有效（防抖后）边沿时间
    volatile u64 m_first_press_us = 0; // 双击窗口内的第一次按下时间；0 = 窗口空闲
    volatile bool m_double_click_pending = false;
    volatile bool m_button_press_pending = false;
    volatile bool m_long_press_pending = false;
    volatile bool m_suppress_long_press = false;

    static void isr_handler(void *arg);
    static void long_press_timer_cb(void *arg);

    void on_isr();
    void on_long_press_timer();
};
} // namespace mlink

#endif
