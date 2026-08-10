#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/usb.hpp"
#include "meguminlink/common.hpp"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include <esp_log.h>

static constexpr char TAG[] = "mlink::usb";

static void tinyusb_event_callback(tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

esp_err_t mlink::cmsis_dap_usb_init()
{
    static bool initialized = false;
    if (initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;

    tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG(tinyusb_event_callback);
    ret = tinyusb_driver_install(&tinyusb_config);
    __check("Failed to initialize TinyUSB driver");

    initialized = true;
    return ESP_OK;

    err:
    return ret;
}