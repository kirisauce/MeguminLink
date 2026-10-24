#pragma once
#ifndef __MEAPROTO_HASH_HPP__
#define __MEAPROTO_HASH_HPP__

#include "meaproto/types.hpp"

#include <span>

namespace meaproto
{
/** 通道表摘要：(id, reliability, max_payload) 的 FNV-1a */
u16 config_hash(std::span<const ChannelSpec> channels) noexcept;
} // namespace meaproto

#endif // __MEAPROTO_HASH_HPP__
