#pragma once
#ifndef __MEGUMINLINK_NVS_FLASH_HPP__
#define __MEGUMINLINK_NVS_FLASH_HPP__

#include "esp_err.h"
#include "esp_now.h" // IWYU pragma: export
#include "meguminlink/common.hpp"
#include "nvs_flash.h" // IWYU pragma: export

#include <cstring>

namespace mlink
{
esp_err_t nvs_flash_init();

/**
 * NVS 键基类：持有 key 名并收敛 nvs_open/nvs_get/nvs_set/nvs_commit/nvs_close 样板。
 * 不直接实例化；使用类型化子类（NvsKeyU8、NvsKeyBlob），子类把值缓存在内存：
 * 读不重复访问 flash，写相同值自动跳过落盘（省 flash 磨损）。
 */
class NvsKey
{
  public:
    explicit NvsKey(const char *key);
    NvsKey(const NvsKey &) = delete;

  protected:
    /** 底层原语：无记录/失败返回 false；store 返回错误码（含 commit） */
    bool raw_load_u8(u8 *value) const;
    esp_err_t raw_store_u8(u8 value) const;
    bool raw_load_blob(void *value, size_t len) const;
    esp_err_t raw_store_blob(const void *value, size_t len) const;

  private:
    const char *m_key;
};

/** u8 型 NVS 键：构造时给默认值；首次访问自动从 flash 加载，无记录用默认值 */
class NvsKeyU8 : public NvsKey
{
  public:
    NvsKeyU8(const char *key, u8 default_value);

    /** 返回当前值：首次调用自动从 NVS 加载（无记录/失败用构造时的默认值），随后缓存 */
    u8 get() const;

    /** 更新缓存并写回 NVS；与缓存相同则跳过落盘（返回 ESP_OK） */
    esp_err_t set(u8 value);

  private:
    const u8 m_default_value;
    mutable u8 m_value = 0;
    mutable bool m_loaded = false;
};

/** blob 型 NVS 键：定长缓存（模板 N = 字节数），语义同 NvsKeyU8 */
template <size_t N> class NvsKeyBlob : public NvsKey
{
  public:
    explicit NvsKeyBlob(const char *key) : NvsKey(key)
    {
    }

    /** 拷贝缓存到 out（len 须 == N）；首次调用自动从 NVS 加载；无记录返回 false */
    bool get(void *out, size_t len) const
    {
        if (!m_loaded)
        {
            m_loaded = raw_load_blob(m_data, N);
        }
        if (!m_loaded || len != N)
        {
            return false;
        }
        memcpy(out, m_data, N);
        return true;
    }

    /** 更新缓存并写回 NVS；与缓存相同则跳过落盘（返回 ESP_OK） */
    esp_err_t set(const void *value, size_t len)
    {
        if (len != N)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (m_loaded && memcmp(value, m_data, N) == 0)
        {
            return ESP_OK; // 值未变化：跳过落盘
        }
        memcpy(m_data, value, N);
        m_loaded = true;
        return raw_store_blob(m_data, N);
    }

  private:
    mutable u8 m_data[N] = {};
    mutable bool m_loaded = false;
};

/** 读取已配对对端的 MAC 地址；不存在时返回 false */
bool peer_mac_load(u8 mac[ESP_NOW_ETH_ALEN]);

/** 持久化对端 MAC 地址（配对完成时调用） */
esp_err_t peer_mac_store(const u8 mac[ESP_NOW_ETH_ALEN]);
} // namespace mlink

#endif
