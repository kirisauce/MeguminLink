#pragma once
#ifndef __MEGUMINLINK_NVS_FLASH_HPP__
#define __MEGUMINLINK_NVS_FLASH_HPP__

#include "esp_err.h"
#include "esp_now.h" // IWYU pragma: export
#include "meguminlink/common.hpp"
#include "nvs_flash.h" // IWYU pragma: export

namespace mlink
{
esp_err_t nvs_flash_init();

/** 读取已配对对端的 MAC 地址；不存在时返回 false */
bool peer_mac_load(u8 mac[ESP_NOW_ETH_ALEN]);

/** 持久化对端 MAC 地址（配对完成时调用） */
esp_err_t peer_mac_store(const u8 mac[ESP_NOW_ETH_ALEN]);
} // namespace mlink

#endif
