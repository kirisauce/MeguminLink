#include "meguminlink/wireless/espnow_transport.hpp"
#include "meaproto/wire.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <cstring>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/task.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::espnow";

namespace
{
/** 发送失败日志的限速间隔 */
constexpr u32 TX_FAIL_LOG_INTERVAL_MS = 1000;
} // namespace

EspNowTransport &EspNowTransport::instance() noexcept
{
    static EspNowTransport transport;
    return transport;
}

esp_err_t EspNowTransport::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    m_tx_mutex = xSemaphoreCreateMutex();
    if (m_tx_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = mlink::nvs_flash_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init() failed");

    ret = esp_netif_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_netif_init() failed");
    ret = esp_event_loop_create_default();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_event_loop_create_default() failed");

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_init() failed");

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_storage() failed");

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_mode() failed");

    wifi_config_t wifi_cfg = {};
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_config() failed");

    ret = esp_wifi_start();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_start() failed");

    // 禁用省电：默认的 MIN_MODEM 会让每条 DAP 往返多 ~0.3ms（A/B 实测）
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_ps() failed: %s", esp_err_to_name(ret));

    ret = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_channel() failed");

    ret = esp_now_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_now_init() failed");

    ret = peer_add(ESP_NOW_BROADCAST);
    ESP_RETURN_ON_ERROR(ret, TAG, "peer_add(broadcast) failed: %s", esp_err_to_name(ret));

    ret = esp_now_register_recv_cb(&EspNowTransport::recv_cb);
    ESP_RETURN_ON_ERROR(ret, TAG, "register recv callback failed: %s", esp_err_to_name(ret));

    ret = esp_now_register_send_cb(&EspNowTransport::send_cb);
    ESP_RETURN_ON_ERROR(ret, TAG, "register send callback failed: %s", esp_err_to_name(ret));

    m_initialized = true;
    return ESP_OK;
}

esp_err_t EspNowTransport::bind(const u8 *peer_mac)
{
    if (peer_mac == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t ret = peer_add(peer_mac);
    if (ret != ESP_OK)
    {
        return ret;
    }

    std::memcpy(m_peer_mac, peer_mac, ESP_NOW_ETH_ALEN);
    m_bound = true;
    return ESP_OK;
}

void EspNowTransport::unbind()
{
    if (!m_bound)
    {
        return;
    }

    (void)peer_remove(m_peer_mac);
    std::memset(m_peer_mac, 0, ESP_NOW_ETH_ALEN);
    m_bound = false;
}

bool EspNowTransport::send(std::span<const u8> data) noexcept
{
    return m_bound && send_raw(m_peer_mac, data);
}

bool EspNowTransport::send_to(const u8 *peer_mac, std::span<const u8> data) noexcept
{
    return send_raw(peer_mac, data);
}

void EspNowTransport::set_receive_handler(meaproto::ReceiveFn callback, void *ctx) noexcept
{
    m_receive_callback = callback;
    m_receive_ctx = ctx;
}

void EspNowTransport::set_non_meaproto_frame_callback(NonMeaprotoFrameFn callback, void *ctx) noexcept
{
    m_non_meaproto_frame_callback = callback;
    m_non_meaproto_frame_ctx = ctx;
}

void EspNowTransport::set_rx_observer_callback(RxObserverFn callback, void *ctx) noexcept
{
    m_rx_observer_callback = callback;
    m_rx_observer_ctx = ctx;
}

u32 EspNowTransport::tx_frames() const noexcept
{
    return m_tx_frames;
}

u32 EspNowTransport::tx_failures() const noexcept
{
    return m_tx_failures;
}

esp_err_t EspNowTransport::peer_add(const u8 *peer_mac)
{
    if (peer_mac == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t EspNowTransport::peer_remove(const u8 *peer_mac)
{
    if (peer_mac == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    const esp_err_t ret = esp_now_del_peer(peer_mac);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_del_peer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool EspNowTransport::send_raw(const u8 *peer_mac, std::span<const u8> data) noexcept
{
    if (!m_initialized || peer_mac == nullptr || data.empty() || data.size() > ESP_NOW_MAX_DATA_LEN_V2 ||
        m_tx_mutex == nullptr)
    {
        return false;
    }

    if (xSemaphoreTake(m_tx_mutex, pdMS_TO_TICKS(1)) != pdTRUE)
    {
        return false;
    }
    const esp_err_t ret = esp_now_send(peer_mac, data.data(), data.size());
    xSemaphoreGive(m_tx_mutex);

    if (ret != ESP_OK)
    {
        ++m_tx_failures;
        return false;
    }

    ++m_tx_frames;
    return true;
}

void EspNowTransport::recv_cb(const esp_now_recv_info_t *info, const u8 *data, int len)
{
    if (info == nullptr || info->src_addr == nullptr || data == nullptr || len <= 0)
    {
        return;
    }

    EspNowTransport &self = instance();
    const usize size = static_cast<usize>(len);
    if (self.m_rx_observer_callback != nullptr)
    {
        const i8 rssi = (info->rx_ctrl != nullptr) ? info->rx_ctrl->rssi : 0;
        self.m_rx_observer_callback(self.m_rx_observer_ctx, static_cast<u16>(size), rssi);
    }

    // 首字节高 4 位是协议版本：meaproto 帧交协议层（只认已绑定 peer），其余交非协议回调
    if (!meaproto::is_protocol_frame(data[0]))
    {
        if (self.m_non_meaproto_frame_callback != nullptr)
        {
            self.m_non_meaproto_frame_callback(self.m_non_meaproto_frame_ctx, info->src_addr, data, size);
        }
        return;
    }

    if (self.m_receive_callback != nullptr && self.m_bound &&
        std::memcmp(info->src_addr, self.m_peer_mac, ESP_NOW_ETH_ALEN) == 0)
    {
        self.m_receive_callback(self.m_receive_ctx, data, size);
    }
}

void EspNowTransport::send_cb([[maybe_unused]] const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        return;
    }

    EspNowTransport &self = instance();
    ++self.m_tx_failures;

    const u32 now = static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (static_cast<i32>(now - self.m_next_fail_log_ms) < 0)
    {
        return;
    }
    self.m_next_fail_log_ms = now + TX_FAIL_LOG_INTERVAL_MS;
    ESP_LOGW(TAG, "ESP-NOW send failed (%u total)", static_cast<unsigned>(self.m_tx_failures));
}
