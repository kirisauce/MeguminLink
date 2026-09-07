#include "meguminlink/led/led_controller.hpp"

#include "esp_log.h"
#include "meguminlink/app.hpp" // Application 完整定义（读取 mode/pairing_active）

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::led";
} // namespace

esp_err_t LedController::init(Application &app)
{
    m_app = &app;

    const esp_err_t ret = m_driver.init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Driver init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

void LedController::update()
{
    // 状态 → 效果映射（集中在本处，改灯语义只需动这里）。
    // 语义按驱动选型分流（各驱动类型不互通，见 led_driver.hpp 选型）：
    //   - GPIO：双灯无颜色，LED1 模式灯 / LED2 配对灯；
    //   - WS2812：单颗可调色灯，用颜色+效果编码所有状态。
    const bool wireless = m_app->mode() == Mode::Wireless;
    const bool pairing = m_app->pairing_active();

#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
    // GPIO 双灯：
    //   LED1 模式灯：Local 常亮 / Wireless 灭
    //   LED2 配对/连接灯：配对中闪烁 / Wireless 已配对常亮 / Local 灭
    m_driver.apply(LedEffects{wireless ? LedEffect::Off : LedEffect::On,
                              pairing    ? LedEffect::Blink
                             : wireless ? LedEffect::On
                                        : LedEffect::Off});
#elif CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    // WS2812 单灯状态映射（颜色 + 效果）：
    //   Local           → 绿色常亮
    //   Wireless 已配对 → 蓝色常亮
    //   Wireless 配对中 → 蓝色呼吸（正弦波淡入淡出）
    //   兜底/异常       → 红色闪烁
    Ws2812LedTarget target;
    if (wireless)
    {
        target = pairing ? Ws2812LedTarget{0, 0, 255, Ws2812LedEffect::Breath}
                         : Ws2812LedTarget{0, 0, 255, Ws2812LedEffect::On};
    }
    else
    {
        target = Ws2812LedTarget{0, 255, 0, Ws2812LedEffect::On};
    }
    m_driver.apply(target);
#endif
}
