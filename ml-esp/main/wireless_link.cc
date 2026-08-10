#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/wireless_link.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "meguminlink/app.hpp"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/common.hpp"
#include <cstring>

using namespace mlink;

static constexpr char TAG[] = "mlink::wireless_link";

static constexpr u8 CHANNEL = 10;

static void link_recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len)
{
    if (recv_info == nullptr || recv_info->src_addr == nullptr)
    {
        return;
    }

    ReceivedPacket rp = {};
    std::memcpy(rp.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    if (rp.packet.deserialize(data, data_len) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to deserialize %d-byte frame from " MACSTR, data_len, MAC2STR(rp.src_mac));
        return;
    }

    Application::get_instance().push_received_packet(rp);
}

static void link_send_cb(const wifi_tx_info_t *send_info, esp_now_send_status_t send_status)
{
    if (send_status != ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGW(TAG, "ESP-NOW send failed");
    }
}

static esp_err_t ensure_broadcast_peer()
{
    if (esp_now_is_peer_exist(ESP_NOW_BROADCAST))
    {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, ESP_NOW_BROADCAST, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register broadcast peer: %s", esp_err_to_name(ret));
    }
    return ret;
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

esp_err_t mlink::wireless_link_init()
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
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        __check("esp_wifi_set_mode() failed");

        wifi_config_t wifi_cfg = {};
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        __check("esp_wifi_set_config() failed");

        ret = esp_wifi_start();
        __check("esp_wifi_start() failed");

        ret = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
        __check("esp_wifi_set_channel() failed");
    }

    ret = esp_now_init();
    __check("esp_now_init() failed");

    ret = ensure_broadcast_peer();
    __check("ensure_broadcast_peer() failed: %s", esp_err_to_name(ret));

    ret = wireless_link_register();
    if (ret != ESP_OK)
        goto err;

    initialized = true;
    return ESP_OK;

err:
    return ret;
}

esp_err_t mlink::wireless_link_peer_add(const u8 *peer_mac)
{
    if (esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add peer " MACSTR ": %s", MAC2STR(peer_mac), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mlink::wireless_link_peer_remove(const u8 *peer_mac)
{
    if (!esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    const esp_err_t ret = esp_now_del_peer(peer_mac);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to remove peer " MACSTR ": %s", MAC2STR(peer_mac), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mlink::wireless_link_send(const u8 *dest_mac, const Packet &packet)
{
    u8 buffer[16];
    const esp_err_t ret = packet.serialize(buffer, sizeof(buffer));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to serialize packet: %s", esp_err_to_name(ret));
        return ret;
    }

    return esp_now_send(dest_mac, buffer, packet.serialized_size());
}

#define __check_size(parser, expected_size)                                                                            \
    if (parser.remaining() < expected_size)                                                                            \
    {                                                                                                                  \
        return ESP_ERR_INVALID_SIZE;                                                                                   \
    }

Packet::Packet()
{
}

size_t Packet::serialized_size() const
{
    switch (packet_type)
    {
    case PacketType::Probe: {
        return 1;
    }
    case PacketType::PairingRequest: {
        return 1 + ESP_NOW_ETH_ALEN;
    }
    case PacketType::PairingAccepted: {
        return 1 + ESP_NOW_ETH_ALEN;
    }
    default: {
        // 与 serialize() 的 ESP_ERR_NOT_SUPPORTED 对齐：未知类型按非法处理
        return 0;
    }
    }
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
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        __check_size(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        __check_size(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    default: {
        break;
    }
    }

    return ESP_OK;
}

esp_err_t Packet::serialize(u8 *buffer, size_t len) const
{
    BinSerializer s(buffer, len);
    __check_size(s, 1);
    s.write_u8(static_cast<u8>(packet_type));

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        __check_size(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        __check_size(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef __check_size
