#include "esp_log.h"
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "sdkconfig.h"

extern "C" void app_main();

void app_main()
{
    ESP_ERROR_CHECK(mlink::nvs_flash_init());

    // get_instance() 延迟构造：构造时（NVS 已初始化）自动完成无线链路
    // 初始化（WiFi/ESP-NOW/无线侧 DAP 执行器）、USB 初始化、模式切换按键
    // 注册，并应用模式（USB 常驻，模式差异在 dap_task 的本地执行/无线
    // 转发分流）；配对由 Wireless 模式内部自动驱动。随后进入主循环。
    mlink::Application::get_instance().run();
}
