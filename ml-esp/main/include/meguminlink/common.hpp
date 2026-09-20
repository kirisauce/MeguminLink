#pragma once
#ifndef __MEGUMINLINK_COMMON_HPP__
#define __MEGUMINLINK_COMMON_HPP__

#include <cstddef>
#include <cstdint>
#include <type_traits>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using usize = std::make_unsigned<size_t>::type;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using isize = std::make_signed<size_t>::type;

using f32 = float;
using f64 = double;

#endif // __MEGUMINLINK_COMMON_HPP__