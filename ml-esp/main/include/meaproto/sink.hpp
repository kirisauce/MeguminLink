#pragma once
#ifndef __MEAPROTO_SINK_HPP__
#define __MEAPROTO_SINK_HPP__

#include "meaproto/types.hpp"

#include <concepts>
#include <span>

namespace meaproto
{
/** 应用侧接收接口：回调运行在 meaproto 任务上下文，只允许入队或拷贝 */
template <typename T>
concept Sink = requires(T sink, ChannelId channel, std::span<const u8> data, DisconnectReason reason) {
    { sink.on_message(channel, data) } -> std::same_as<void>;
    { sink.on_connected() } -> std::same_as<void>;
    { sink.on_disconnected(reason) } -> std::same_as<void>;
};
} // namespace meaproto

#endif // __MEAPROTO_SINK_HPP__
