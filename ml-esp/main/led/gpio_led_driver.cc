#include "meguminlink/led/gpio_led_driver.hpp"

#include "esp_log.h"
#include "sdkconfig.h"

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::led";
} // namespace

GpioLedDriver::Effects GpioLedDriver::scene_effects(LedScene scene)
{
    switch (scene)
    {
    case LedScene::Local:
        // 本地调试：模式灯常亮，配对灯灭
        return Effects{Effect::On, Effect::Off};
    case LedScene::Wireless:
        // 无线调试已配对：模式灯灭，连接灯常亮
        return Effects{Effect::Off, Effect::On};
    case LedScene::Pairing:
        // 配对流程中：连接灯闪烁
        return Effects{Effect::Off, Effect::Blink};
    default:
        // 更新模式的各个阶段（空闲/接收/校验/成功/失败）：两盏灯都灭
        return Effects{Effect::Off, Effect::Off};
    }
}

esp_err_t GpioLedDriver::init()
{
    m_active_low = CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_ACTIVE_LOW;

    // 先置为“灭”电平再配置输出，避免配置瞬间的毛刺
    for (int i = 0; i < 2; i++)
    {
        gpio_set_level(m_io[i], m_active_low ? 1 : 0);
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << m_io[0]) | (1ULL << m_io[1]);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 闪烁用周期定时器：半周期 = BLINK_PERIOD_MS，Blink 灯每 tick 翻转一次
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = timer_cb;
    timer_args.arg = this;
    timer_args.name = "mlink_led_blink";
    ret = esp_timer_create(&timer_args, &m_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(m_timer, CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_BLINK_PERIOD_MS * 1000u);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "GPIO driver: LED1=GPIO%d LED2=GPIO%d active_low=%d blink=%d ms", m_io[0], m_io[1], m_active_low,
             CONFIG_MEGUMINLINK_LED_DRIVER_GPIO_BLINK_PERIOD_MS);
    return ESP_OK;
}

void GpioLedDriver::apply_impl(const LedRequest &request)
{
    const Effects effects = scene_effects(request.scene);

    portENTER_CRITICAL(&m_lock);
    m_effects = effects;
    // 立即同步静态效果；Blink 的相位由定时器维护（不重置），
    // 使闪烁节奏与上层下发频率完全解耦
    for (int i = 0; i < 2; i++)
    {
        switch (effects[i])
        {
        case Effect::On:
            write_level(i, true);
            break;
        case Effect::Off:
            write_level(i, false);
            break;
        case Effect::Blink:
            break;
        }
    }
    portEXIT_CRITICAL(&m_lock);
}

void GpioLedDriver::timer_cb(void *arg)
{
    auto *self = static_cast<GpioLedDriver *>(arg);

    portENTER_CRITICAL(&self->m_lock);
    for (int i = 0; i < 2; i++)
    {
        if (self->m_effects[i] == Effect::Blink)
        {
            self->m_level[i] = !self->m_level[i];
            gpio_set_level(self->m_io[i], self->m_level[i]);
        }
    }
    portEXIT_CRITICAL(&self->m_lock);
}

void GpioLedDriver::write_level(int index, bool lit)
{
    const bool level = m_active_low ? !lit : lit; // 低电平点亮：点亮 → 输出低
    m_level[index] = level;
    gpio_set_level(m_io[index], level);
}
