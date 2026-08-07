#include "meguminlink/nvs_flash.hpp"
#include "esp_log.h"

static constexpr char TAG[] = "mlink::nvs_flash";

static constexpr char NVS_NAMESPACE[] = "mlink";
static constexpr char KEY_PEER_MAC[] = "peer_mac";

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

bool mlink::peer_mac_load(u8 mac[ESP_NOW_ETH_ALEN])
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }

    size_t len = ESP_NOW_ETH_ALEN;
    const esp_err_t ret = nvs_get_blob(handle, KEY_PEER_MAC, mac, &len);
    nvs_close(handle);

    return ret == ESP_OK && len == ESP_NOW_ETH_ALEN;
}

esp_err_t mlink::peer_mac_store(const u8 mac[ESP_NOW_ETH_ALEN])
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, KEY_PEER_MAC, mac, ESP_NOW_ETH_ALEN);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to store peer MAC: %s", esp_err_to_name(ret));
    }
    return ret;
}
