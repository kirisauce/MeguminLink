#include "esp_log.h"
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless_link.hpp"
#include "sdkconfig.h"

extern "C" void app_main();

void app_main()
{
    ESP_ERROR_CHECK(mlink::nvs_flash_init());

    ESP_ERROR_CHECK(mlink::wireless_link_init());

    // get_instance() 延迟构造：构造时（NVS 已初始化）自动完成 USB 初始化、
    // 无线侧 DAP 执行器初始化、模式切换按键注册，并应用模式（USB 常驻，
    // 模式差异在 dap_task 的本地执行/无线转发分流）；配对由 Wireless 模式
    // 内部自动驱动。随后进入主循环。
    mlink::Application::get_instance().run();
}
