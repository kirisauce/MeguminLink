#include "meguminlink/wireless_link.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/nvs_flash.hpp"

using namespace mlink;

static constexpr char TAG[] = "mlink::wireless_link";

#define __check(...)                                                                                                   \
    if (ret != ESP_OK)                                                                                                 \
    {                                                                                                                  \
        ESP_LOGE(TAG, __VA_ARGS__);                                                                                    \
        goto err;                                                                                                      \
    }

static void link_recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len)
{
}

static void link_send_cb(const wifi_tx_info_t *send_info, esp_now_send_status_t send_status)
{
}

static inline esp_err_t wireless_link_register()
{
    esp_err_t ret;

    ret = esp_now_register_recv_cb(link_recv_cb);
    __check("Failed to register ESP-NOW receive callback: %s", esp_err_to_name(ret));

    ret = esp_now_register_send_cb(link_send_cb);
    __check("Failed to register ESP-NOW send callback: %s", esp_err_to_name(ret));

    return ESP_OK;

err:
    return ret;
}

esp_err_t wireless_link_init()
{
    static bool initialized = false;
    if (initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;
    ret = mlink::nvs_flash_init();
    __check("mlink::nvs_flash_init() failed");

    ret = esp_netif_init();
    __check("esp_netif_init() failed");
    ret = esp_event_loop_create_default();
    __check("esp_event_loop_create_default() failed");

    {
        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&wifi_init_cfg);
        __check("esp_wifi_init() failed");
        ESP_LOGI(TAG, "WiFi initialized");

        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        __check("esp_wifi_set_storage() failed");
    }

    // ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    // ESP_LOGI(TAG, "WiFi promiscuous mode was enabled");

    {
        wifi_config_t wifi_cfg = {};
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        __check("esp_wifi_set_config() failed");
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        __check("esp_wifi_set_mode() failed");
        ret = esp_wifi_start();
        __check("esp_wifi_start() failed");
        ret = esp_wifi_set_channel(10, WIFI_SECOND_CHAN_NONE);
        __check("esp_wifi_set_channel() failed");
    }

    ret = esp_now_init();
    __check("esp_now_init() failed");

    ret = wireless_link_register();
    if (ret != ESP_OK)
        goto err;

    initialized = true;
    return ESP_OK;

err:
    return ret;
}

#undef __check

#define __check_size(parser, expected_size)                                                                            \
    if (parser.remaining() < expected_size)                                                                            \
    {                                                                                                                  \
        return ESP_ERR_INVALID_SIZE;                                                                                   \
    }

Packet::Packet()
{
}

esp_err_t Packet::deserialize(const u8 *data, size_t len)
{
    BinDeserializer p(data, data + len);

    __check_size(p, 1);
    packet_type = static_cast<PacketType>(p.read_u8());

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    default: {
        break;
    }
    }

    return ESP_OK;
}

esp_err_t Packet::serialize(u8 *buffer, size_t len)
{
    BinSerializer s(buffer, len);
    __check_size(s, 1);
    s.write_u8(static_cast<u8>(packet_type));

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef __check_size