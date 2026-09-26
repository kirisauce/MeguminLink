#include "meguminlink/nvs_flash.hpp"

#include <esp_check.h>
#include <esp_log.h>

static constexpr char TAG[] = "mlink::nvs_flash";

namespace
{
constexpr char KEY_PEER_MAC[] = "peer_mac";
} // namespace

mlink::NvsKeyBlob<ESP_NOW_ETH_ALEN> &mlink::peer_mac_key()
{
    static mlink::NvsKeyBlob<ESP_NOW_ETH_ALEN> key(KEY_PEER_MAC);
    return key;
}

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

bool mlink::NvsKey::raw_load_string(char *out, size_t cap) const
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }
    size_t out_len = cap;
    const esp_err_t ret = nvs_get_str(handle, m_key, out, &out_len);
    nvs_close(handle);
    return ret == ESP_OK;
}

esp_err_t mlink::NvsKey::raw_store_string(const char *value) const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(ret));

    ret = nvs_set_str(handle, m_key, value);
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

mlink::NvsWriteBatch::NvsWriteBatch(const char *nvs_namespace)
{
    const esp_err_t ret = nvs_open(nvs_namespace, NVS_READWRITE, &m_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", nvs_namespace, esp_err_to_name(ret));
        return;
    }
    m_open = true;
}

mlink::NvsWriteBatch::~NvsWriteBatch()
{
    // 没显式 commit 就析构：本批的写入不落盘（nvs_close 不会提交未提交的更改）
    if (m_open)
    {
        nvs_close(m_handle);
    }
}

bool mlink::NvsWriteBatch::ready() const
{
    return m_open;
}

esp_err_t mlink::NvsWriteBatch::set_u8(const char *key, u8 value)
{
    return m_open ? nvs_set_u8(m_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t mlink::NvsWriteBatch::set_u16(const char *key, u16 value)
{
    return m_open ? nvs_set_u16(m_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t mlink::NvsWriteBatch::set_u32(const char *key, u32 value)
{
    return m_open ? nvs_set_u32(m_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t mlink::NvsWriteBatch::commit()
{
    if (!m_open)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_commit(m_handle);
}
