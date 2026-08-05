#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_wifi.h" // IWYU pragma: export
#include "meguminlink/common.hpp" // IWYU pragma: keep

namespace mlink {
    esp_err_t wireless_link_init();
}

#endif