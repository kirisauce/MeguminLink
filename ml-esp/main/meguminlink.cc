#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/update_mode.hpp"
#endif

extern "C" void app_main();

void app_main()
{
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    // 更新模式请求优先于一切初始化（进入请求在本调用里读出并清除）：该分支不
    // 初始化 NVS、无线、串口桥与 Application，只安装一个 vendor 接口的 USB
    // 并显示渐变灯效。见 meguminlink/usb/update_mode.hpp。
    if (mlink::update_mode::consume_request())
    {
        mlink::UpdateMode::get_instance().run(); // 不返回
    }
#endif

    ESP_ERROR_CHECK(mlink::nvs_flash_init());

    // get_instance() 延迟构造：构造时（NVS 已初始化）自动完成无线链路
    // 初始化（WiFi/ESP-NOW/无线侧 DAP 执行器）、USB 初始化、模式切换按键
    // 注册，并应用模式（USB 常驻，模式差异在 dap_task 的本地执行/无线
    // 转发分流）；配对由 Wireless 模式内部自动驱动。
    mlink::Application &app = mlink::Application::get_instance();

    // 全部子系统已就绪：确认这一版固件可用，取消引导程序的下一次回滚
    // （见 app.hpp）。随后进入主循环。
    mlink::ota_confirm_running_app();

    app.run();
}
