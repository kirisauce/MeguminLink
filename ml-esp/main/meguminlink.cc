#include "meguminlink/app.hpp"
#include "meguminlink/common.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi_types_generic.h"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless_link.hpp"
#include <cstring>

static constexpr char TAG[] = "mlink::main";

// Receiver MAC
constexpr static u8 RECEIVER_MAC[6] = {0x7c, 0xdf, 0xa1, 0xbe, 0xde, 0xc8};

// Sender MAC
constexpr static u8 SENDER_MAC[6] = {0xb4, 0x3a, 0x45, 0x4f, 0x70, 0xa4};

extern "C" void app_main();

void app_main()
{
    ESP_ERROR_CHECK(mlink::nvs_flash_init());

    ESP_ERROR_CHECK(mlink::wireless_link_init());

    {
        auto recv_cb = [](const esp_now_recv_info_t *recv_info, const u8 *data, int data_len) {
            ESP_LOGI(TAG, "Received %d bytes of data", data_len);
        };

        ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

        esp_now_peer_info_t peer = {};
        std::memcpy(&peer.peer_addr, &RECEIVER_MAC, ESP_NOW_ETH_ALEN);
        peer.channel = 10;
        peer.encrypt = false;
        peer.ifidx = WIFI_IF_STA;
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));

        // esp_now_rate_config_t peer_rate_config;
        // peer_rate_config.rate = WIFI_PHY_RATE_MCS7_SGI;
        // peer_rate_config.dcm = true;
        // peer_rate_config.phymode = WIFI_PHY_MODE_LR;
        // peer_rate_config.ersu = true;
        // ESP_ERROR_CHECK(esp_now_set_peer_rate_config(RECEIVER_MAC, &peer_rate_config));
    }

    auto &app = mlink::Application::get_instance();

    while (true)
    {
        app.run();
    }
}
