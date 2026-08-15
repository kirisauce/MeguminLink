#include "meguminlink/led/led_controller.hpp"

#include "esp_log.h"
#include "meguminlink/app.hpp" // Application 完整定义（读取 mode/pairing_active）
#include "sdkconfig.h"

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
    // 状态 → 效果映射（集中在本处，改灯语义只需动这里）：
    //   LED1 模式灯：Local 常亮 / Wireless 灭
    //   LED2 配对/连接灯：配对中闪烁 / Wireless 已配对常亮 / Local 灭
    const bool wireless = m_app->mode() == Mode::Wireless;
    const bool pairing = m_app->pairing_active();

    m_driver.apply(LedEffects{wireless ? LedEffect::Off : LedEffect::On,
                              pairing ? LedEffect::Blink
                                      : wireless ? LedEffect::On : LedEffect::Off});
}
