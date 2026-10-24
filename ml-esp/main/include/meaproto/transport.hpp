#pragma once
#ifndef __MEAPROTO_TRANSPORT_HPP__
#define __MEAPROTO_TRANSPORT_HPP__

#include "meaproto/types.hpp"

#include <concepts>
#include <cstddef>
#include <span>

namespace meaproto
{
/** 接收回调，实现方只做拷贝入队 */
using ReceiveFn = void (*)(void *ctx, const u8 *data, usize len);

/** 一条已绑定的不可靠连接 */
template <typename T>
concept Transport = requires(T transport, std::span<const u8> data, ReceiveFn fn, void *ctx) {
    { transport.mtu() } noexcept -> std::convertible_to<usize>;
    { transport.send(data) } noexcept -> std::same_as<bool>;
    { transport.set_receive_handler(fn, ctx) } noexcept;
};
} // namespace meaproto

#endif // __MEAPROTO_TRANSPORT_HPP__
