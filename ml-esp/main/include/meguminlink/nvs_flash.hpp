#pragma once
#ifndef __MEGUMINLINK_NVS_FLASH_HPP__
#define __MEGUMINLINK_NVS_FLASH_HPP__

#include "esp_err.h"
#include "nvs_flash.h" // IWYU pragma: export

namespace mlink {
    esp_err_t nvs_flash_init();
}

#endif