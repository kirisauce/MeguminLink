#include "meguminlink/wireless/link_config.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <esp_log.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::link_config";

namespace
{
// NVS 键名上限 15 字符
constexpr char KEY_RTO_INIT[] = "link_rto_init";
constexpr char KEY_RTO_MAX[] = "link_rto_max";
constexpr char KEY_ACK_DELAY[] = "link_ack_delay";
constexpr char KEY_KEEPALIVE[] = "link_keepalive";

/** 时序类字段的合法性：与 Core::apply_config 的拒绝条件一致 */
bool timing_valid(const meaproto::Config &config)
{
    return config.rto_initial_ms >= 1 && config.rto_max_ms >= config.rto_initial_ms &&
           config.keepalive_interval_ms >= 1 && config.connect_timeout_ms >= 1 && config.send_quota_per_tick >= 1;
}
} // namespace

meaproto::Config mlink::link_config_load()
{
    meaproto::Config config;

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return config; // 无记录：默认值
    }

    // 逐项读：某个键缺失就沿用默认值，其余照常恢复
    u16 value = 0;
    if (nvs_get_u16(handle, KEY_RTO_INIT, &value) == ESP_OK)
    {
        config.rto_initial_ms = value;
    }
    if (nvs_get_u16(handle, KEY_RTO_MAX, &value) == ESP_OK)
    {
        config.rto_max_ms = value;
    }
    if (nvs_get_u16(handle, KEY_ACK_DELAY, &value) == ESP_OK)
    {
        config.ack_delay_ms = value;
    }
    if (nvs_get_u16(handle, KEY_KEEPALIVE, &value) == ESP_OK)
    {
        config.keepalive_interval_ms = value;
    }

    nvs_close(handle);

    if (!timing_valid(config))
    {
        ESP_LOGW(TAG, "stored link config invalid, falling back to defaults");
        return meaproto::Config{};
    }
    return config;
}

esp_err_t mlink::link_config_store(const meaproto::Config &config)
{
    if (!timing_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    NvsWriteBatch batch(NVS_NAMESPACE);
    if (!batch.ready())
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 整份参数走同一个 commit：掉电重启不会读到改了一半的混合状态
    esp_err_t ret = batch.set_u16(KEY_RTO_INIT, config.rto_initial_ms);
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_RTO_MAX, config.rto_max_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_ACK_DELAY, config.ack_delay_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.set_u16(KEY_KEEPALIVE, config.keepalive_interval_ms);
    }
    if (ret == ESP_OK)
    {
        ret = batch.commit();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to store link config: %s", esp_err_to_name(ret));
    }
    return ret;
}
