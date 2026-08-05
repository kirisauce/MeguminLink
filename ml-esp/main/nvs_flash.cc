#include "meguminlink/nvs_flash.hpp"
#include "esp_log.h"

static constexpr char TAG[] = "mlink::nvs_flash";

esp_err_t mlink::nvs_flash_init()
{
    static bool initialized = false;
    if (initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;
    ret = ::nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ret = nvs_flash_erase();
        if (ret != ESP_OK)
            goto err;

        ret = nvs_flash_init();
    }
    if (ret != ESP_OK)
        goto err;
    ESP_LOGI(TAG, "NVS Flash initialized");

    initialized = true;
    return ESP_OK;

err:
    return ret;
}