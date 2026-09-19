#include "meguminlink/nvs_flash.hpp"

#include <esp_check.h>
#include <esp_log.h>

static constexpr char TAG[] = "mlink::nvs_flash";

namespace
{
constexpr char NVS_NAMESPACE[] = "mlink";
constexpr char KEY_PEER_MAC[] = "peer_mac";

// 模块级常驻的 peer MAC 键：缓存跨 peer_mac_load/store 调用共享
mlink::NvsKeyBlob<ESP_NOW_ETH_ALEN> g_peer_mac_key(KEY_PEER_MAC);
} // namespace

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

mlink::NvsKey::NvsKey(const char *key) : m_key(key)
{
}

bool mlink::NvsKey::raw_load_u8(u8 *value) const
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }
    const esp_err_t ret = nvs_get_u8(handle, m_key, value);
    nvs_close(handle);
    return ret == ESP_OK;
}

esp_err_t mlink::NvsKey::raw_store_u8(u8 value) const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));

    ret = nvs_set_u8(handle, m_key, value);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

bool mlink::NvsKey::raw_load_blob(void *value, size_t len) const
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }
    size_t out_len = len;
    const esp_err_t ret = nvs_get_blob(handle, m_key, value, &out_len);
    nvs_close(handle);
    return ret == ESP_OK && out_len == len;
}

esp_err_t mlink::NvsKey::raw_store_blob(const void *value, size_t len) const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));

    ret = nvs_set_blob(handle, m_key, value, len);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t mlink::NvsKey::raw_erase() const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));

    ret = nvs_erase_key(handle, m_key);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        ret = ESP_OK; // 本来就没有记录：删除已达目的
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

mlink::NvsKeyU8::NvsKeyU8(const char *key, u8 default_value) : NvsKey(key), m_default_value(default_value)
{
}

u8 mlink::NvsKeyU8::get() const
{
    if (!m_loaded)
    {
        if (!raw_load_u8(&m_value))
        {
            m_value = m_default_value; // 无记录 / 失败 → 默认值
        }
        m_loaded = true;
    }
    return m_value;
}

esp_err_t mlink::NvsKeyU8::set(u8 value)
{
    if (m_loaded && value == m_value)
    {
        return ESP_OK; // 值未变化：跳过落盘
    }
    m_value = value;
    m_loaded = true;
    return raw_store_u8(value);
}

esp_err_t mlink::NvsKeyU8::erase()
{
    const esp_err_t ret = raw_erase();
    if (ret == ESP_OK)
    {
        // 缓存失效并回到默认值：下次 get() 与首次访问语义一致
        m_value = m_default_value;
        m_loaded = false;
    }
    return ret;
}

bool mlink::peer_mac_load(u8 mac[ESP_NOW_ETH_ALEN])
{
    return g_peer_mac_key.get(mac, ESP_NOW_ETH_ALEN);
}

esp_err_t mlink::peer_mac_store(const u8 mac[ESP_NOW_ETH_ALEN])
{
    const esp_err_t ret = g_peer_mac_key.set(mac, ESP_NOW_ETH_ALEN);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to store peer MAC: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mlink::peer_mac_erase()
{
    const esp_err_t ret = g_peer_mac_key.erase();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase peer MAC: %s", esp_err_to_name(ret));
    }
    return ret;
}
