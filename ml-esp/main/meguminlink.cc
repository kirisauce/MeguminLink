#include "esp_log.h"
#include "esp_mac.h"
#include "meguminlink/app.hpp"
#include "meguminlink/common.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless_link.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb.hpp"
#endif

static constexpr char TAG[] = "mlink::main";

extern "C" void app_main();

void app_main()
{
    ESP_ERROR_CHECK(mlink::nvs_flash_init());

    ESP_ERROR_CHECK(mlink::wireless_link_init());

    auto &app = mlink::Application::get_instance();

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    mlink::cmsis_dap_usb_init();
#endif

    // 已存在配对记录则直接进入 Idle，否则上电自动进入配对模式
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (mlink::peer_mac_load(peer_mac))
    {
        ESP_LOGI(TAG, "Already paired with " MACSTR ", skipping pairing", MAC2STR(peer_mac));
    }
    else
    {
        ESP_LOGI(TAG, "No paired device found, entering pairing mode");
        app.set_state(mlink::ApplicationState::Pairing);
    }

    while (true)
    {
        app.run();
    }
}
