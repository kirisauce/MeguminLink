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
/** NVS 命名空间（peer MAC 与链路参数共用） */
inline constexpr char NVS_NAMESPACE[] = "mlink";

esp_err_t nvs_flash_init();

/**
 * NVS 键基类：持有 key 名并收敛 nvs_open/nvs_get/nvs_set/nvs_commit/nvs_close 样板。
 * 不直接实例化；使用类型化子类（NvsKeyU8、NvsKeyBlob、NvsKeyString），子类把值缓存
 * 在内存：读不重复访问 flash，写相同值自动跳过落盘（省 flash 磨损）。
 */
class NvsKey
{
  public:
    explicit NvsKey(const char *key);
    NvsKey(const NvsKey &) = delete;

  protected:
    /** 底层原语：无记录/失败返回 false；store/erase 返回错误码（含 commit） */
    bool raw_load_u8(u8 *value) const;
    esp_err_t raw_store_u8(u8 value) const;
    bool raw_load_blob(void *value, size_t len) const;
    esp_err_t raw_store_blob(const void *value, size_t len) const;
    /** 读取变长字符串到 out（容量 cap，含结尾 NUL）；无记录/失败返回 false */
    bool raw_load_string(char *out, size_t cap) const;
    /** 写入字符串（含结尾 NUL） */
    esp_err_t raw_store_string(const char *value) const;
    /** 删除键；键本来就不存在视为成功（删除已达目的） */
    esp_err_t raw_erase() const;

  private:
    const char *m_key;
};

namespace detail
{
/** 按目标容量截断拷贝并补结尾 NUL（cap 为 0 时不做任何事） */
inline void copy_cstr(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
    {
        return;
    }
    const size_t len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}
} // namespace detail

/** u8 型 NVS 键：构造时给默认值；首次访问自动从 flash 加载，无记录用默认值 */
class NvsKeyU8 : public NvsKey
{
  public:
    NvsKeyU8(const char *key, u8 default_value);

    /** 返回当前值：首次调用自动从 NVS 加载（无记录/失败用构造时的默认值），随后缓存 */
    u8 get() const;

    /** 更新缓存并写回 NVS；与缓存相同则跳过落盘（返回 ESP_OK） */
    esp_err_t set(u8 value);

    /** 删除记录并让缓存回到构造时的默认值（返回 ESP_OK 表示下次 get() 即默认值） */
    esp_err_t erase();

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

    /**
     * 零拷贝访问：返回指向内部缓存的指针，首次调用自动从 NVS 加载（此后读不再
     * 访问 flash）。指针在键对象生命周期内始终有效，写/擦除会原地更新其内容；
     * 无记录或加载失败返回 nullptr。
     */
    const u8 *cached() const
    {
        if (!m_loaded)
        {
            m_loaded = raw_load_blob(m_data, N);
        }
        return m_loaded ? m_data : nullptr;
    }

    /** 拷贝缓存到 out（len 须 == N）；首次调用自动从 NVS 加载；无记录返回 false */
    bool get(void *out, size_t len) const
    {
        const u8 *view = cached();
        if (view == nullptr || len != N)
        {
            return false;
        }
        memcpy(out, view, N);
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

    /** 删除记录并清空缓存（返回 ESP_OK 表示下次 cached() 返回 nullptr） */
    esp_err_t erase()
    {
        const esp_err_t ret = raw_erase();
        if (ret == ESP_OK)
        {
            memset(m_data, 0, N);
            m_loaded = false;
        }
        return ret;
    }

  private:
    mutable u8 m_data[N] = {};
    mutable bool m_loaded = false;
};

/**
 * 字符串型 NVS 键：定长缓存（模板 N = 不含结尾 NUL 的最大字符数），
 * 语义同 NvsKeyU8——首次访问自动从 flash 加载，无记录用构造时的默认值。
 */
template <size_t N> class NvsKeyString : public NvsKey
{
  public:
    NvsKeyString(const char *key, const char *default_value) : NvsKey(key), m_default_value(default_value)
    {
    }

    /** 拷贝当前值到 out（容量须 ≥ N + 1）；首次调用自动加载 */
    void get(char *out, size_t cap) const
    {
        if (!m_loaded)
        {
            if (!raw_load_string(m_data, sizeof(m_data)))
            {
                detail::copy_cstr(m_data, sizeof(m_data), m_default_value); // 无记录/失败 → 默认值
            }
            m_loaded = true;
        }
        detail::copy_cstr(out, cap, m_data);
    }

    /** 更新缓存并写回 NVS；与缓存相同则跳过落盘；超长返回 ESP_ERR_INVALID_ARG */
    esp_err_t set(const char *value)
    {
        if (value == nullptr || strlen(value) > N)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (m_loaded && strcmp(value, m_data) == 0)
        {
            return ESP_OK; // 值未变化：跳过落盘
        }
        detail::copy_cstr(m_data, sizeof(m_data), value);
        m_loaded = true;
        return raw_store_string(value);
    }

    /** 删除记录并让缓存回到构造时的默认值（返回 ESP_OK 表示下次 get() 即默认值） */
    esp_err_t erase()
    {
        const esp_err_t ret = raw_erase();
        if (ret == ESP_OK)
        {
            detail::copy_cstr(m_data, sizeof(m_data), m_default_value);
            m_loaded = true;
        }
        return ret;
    }

  private:
    const char *m_default_value;
    mutable char m_data[N + 1] = {};
    mutable bool m_loaded = false;
};

/**
 * NVS 写入批：把一组相互关联的键放进同一个 commit。
 *
 * NVS 只保证"一次 nvs_commit() 内的写入要么全部生效、要么全部不生效"，逐键各自
 * commit 的话，掉电重启可能读到改了一半的混合状态。整份链路参数属于必须一致的一
 * 组，因此走这个批；只读路径不需要它。
 */
class NvsWriteBatch
{
  public:
    explicit NvsWriteBatch(const char *nvs_namespace);
    NvsWriteBatch(const NvsWriteBatch &) = delete;
    NvsWriteBatch &operator=(const NvsWriteBatch &) = delete;
    ~NvsWriteBatch();

    /** 命名空间是否已成功打开（未打开时所有写入都返回失败） */
    bool ready() const;

    esp_err_t set_u8(const char *key, u8 value);
    esp_err_t set_u16(const char *key, u16 value);
    esp_err_t set_u32(const char *key, u32 value);

    /** 提交：本批的全部写入在这一刻原子落盘 */
    esp_err_t commit();

  private:
    nvs_handle_t m_handle = 0;
    bool m_open = false;
};

/**
 * peer MAC 键的访问点：缓存跨调用共享（实例在 nvs_flash.cc 内按需构造，首次访问
 * 自动从 NVS 加载，此后读不再访问 flash、写相同值跳过落盘）。
 *
 * 读走零拷贝的 cached()：返回指向内部缓存的指针，无记录返回 nullptr；
 * set(mac, ESP_NOW_ETH_ALEN) 持久化（配对完成时调用，相同值跳过落盘）；erase()
 * 删除记录（清除配对 / 恢复出厂设置时调用，键本来不存在视为成功）。
 */
NvsKeyBlob<ESP_NOW_ETH_ALEN> &peer_mac_key();
} // namespace mlink

#endif
