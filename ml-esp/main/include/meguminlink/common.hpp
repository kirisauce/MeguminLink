#pragma once
#ifndef __MEGUMINLINK_COMMON_HPP__
#define __MEGUMINLINK_COMMON_HPP__

#ifdef __MEGUMINLINK_EXPORT_MACROS__
#define MLINK_CHECK(...)                                                                                               \
    if (ret != ESP_OK)                                                                                                 \
    {                                                                                                                  \
        ESP_LOGE(TAG, __VA_ARGS__);                                                                                    \
        goto err;                                                                                                      \
    }
#endif

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;

#endif // __MEGUMINLINK_COMMON_HPP__