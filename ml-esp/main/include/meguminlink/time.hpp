#pragma once
#ifndef __MEGUMINLINK_TIME_HPP__
#define __MEGUMINLINK_TIME_HPP__

#include "meguminlink/common.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace mlink
{
/** 单调毫秒时基：FreeRTOS tick 换算 */
inline u32 now_ms()
{
    return static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
} // namespace mlink

#endif
