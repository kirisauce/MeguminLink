#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_CONFIG_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_CONFIG_HPP__

#include "meaproto/config.hpp"

#include <esp_err.h>

namespace mlink
{
/**
 * 链路参数的持久化：只存运行期可改的时序类字段（RTO、延迟确认、保活）。
 *
 * 尺寸类字段在构造时按存储切分，改它需要重启，因此不进 NVS。
 */
meaproto::Config link_config_load();

/** 整份参数一次 commit；写入前按 apply_config 的规则校验 */
esp_err_t link_config_store(const meaproto::Config &config);
} // namespace mlink

#endif
