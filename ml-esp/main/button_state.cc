#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/button_state.hpp"

#include "esp_log.h"
#include "hal/gpio_ll.h" // gpio_ll_get_level：static inline 寄存器读，ISR 安全

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::button";
} // namespace

esp_err_t ButtonState::init(gpio_num_t gpio)
{
    m_gpio = gpio;

    // 声明置于所有 __check(goto err) 之前，避免跨初始化跳转
    esp_timer_create_args_t timer_args = {};

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE; // 默认 GPIO0（BOOT 键）：内部上拉，按下拉低
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE; // 按下/松开都触发：松开需取消长按定时器

    esp_err_t ret = gpio_config(&cfg);
    __check("gpio_config(GPIO%d) failed: %s", gpio, esp_err_to_name(ret));

    // gpio_install_isr_service 全局唯一，重复调用返回 ESP_ERR_INVALID_STATE；
    // 本模块是项目中第一个使用 GPIO ISR 服务的组件（调用前已确认）。
    ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    __check("gpio_install_isr_service failed: %s", esp_err_to_name(ret));

    // 长按一次性定时器：按下边沿启动，到期仍按住则判长按。
    // 必须先于 ISR 注册创建，保证 ISR 触发时 handle 有效。
    timer_args.callback = long_press_timer_cb;
    timer_args.arg = this;
    timer_args.name = "mlink_long_press";
    ret = esp_timer_create(&timer_args, &m_long_press_timer);
    __check("esp_timer_create(long_press) failed: %s", esp_err_to_name(ret));

    ret = gpio_isr_handler_add(gpio, isr_handler, this);
    __check("gpio_isr_handler_add(GPIO%d) failed: %s", gpio, esp_err_to_name(ret));

    ESP_LOGI(TAG, "Mode switch on GPIO%d (double click %d ms, long press %d ms)", gpio,
             CONFIG_MEGUMINLINK_MODE_SWITCH_DOUBLE_CLICK_MS, CONFIG_MEGUMINLINK_MODE_SWITCH_LONG_PRESS_MS);
    return ESP_OK;

err:
    return ret;
}

bool ButtonState::consume_double_click()
{
    if (m_double_click_pending)
    {
        m_double_click_pending = false;
        return true;
    }
    return false;
}

bool ButtonState::consume_button_press()
{
    if (m_button_press_pending)
    {
        m_button_press_pending = false;
        return true;
    }
    return false;
}

bool ButtonState::consume_long_press()
{
    if (m_long_press_pending)
    {
        m_long_press_pending = false;
        return true;
    }
    return false;
}

void ButtonState::set_long_press_suppressed(bool suppressed)
{
    m_suppress_long_press = suppressed;
}

void IRAM_ATTR ButtonState::isr_handler(void *arg)
{
    static_cast<ButtonState *>(arg)->on_isr();
}

void IRAM_ATTR ButtonState::on_isr()
{
    const u64 now = esp_timer_get_time();

    // ANYEDGE：读电平区分按下/松开（gpio_ll_get_level 为内联寄存器读，ISR 安全；
    // 不能用 gpio_get_level，它在 IDF v6 是非 inline 的 flash 函数）
    const bool now_pressed = gpio_ll_get_level(&GPIO, m_gpio) == 0;

    if (!now_pressed)
    {
        // 松开：取消长按定时器。不做防抖——即使极短单击（按下后立即松开）
        // 也必须取消，否则定时器到期会把"周期性点击"误判成长按。
        esp_timer_stop(m_long_press_timer);
        return;
    }

    // 按下：防抖后识别事件
    if (now - m_last_edge_us < DEBOUNCE_US)
    {
        return;
    }
    m_last_edge_us = now;

    // 任意一次有效按下 → 单次按下事件（具体含义由上层决定）
    m_button_press_pending = true;

    // 双击窗口判断：窗口内第二次按下 → 双击事件；复位 first_press 避免三击连判
    const u64 first = m_first_press_us;
    if (first != 0 && now - first <= CONFIG_MEGUMINLINK_MODE_SWITCH_DOUBLE_CLICK_MS * 1000ULL)
    {
        m_double_click_pending = true;
        m_first_press_us = 0;
    }
    else
    {
        // 第一次按下，或上一次窗口已过期（自然成为新的第一次按下）
        m_first_press_us = now;
    }

    // 长按识别：按下启动定时器，松开即取消（见上）。
    // 被抑制时跳过：该次按下只报单次按下事件，行为由上层另行解释。
    if (!m_suppress_long_press)
    {
        esp_timer_start_once(m_long_press_timer, CONFIG_MEGUMINLINK_MODE_SWITCH_LONG_PRESS_MS * 1000ULL);
    }
}

void ButtonState::long_press_timer_cb(void *arg)
{
    static_cast<ButtonState *>(arg)->on_long_press_timer();
}

void ButtonState::on_long_press_timer()
{
    // 定时器只在"按下后持续未松开"时才会到期（松开即 esp_timer_stop），
    // 到期必为真长按，无需再校验电平。
    m_long_press_pending = true;
}
