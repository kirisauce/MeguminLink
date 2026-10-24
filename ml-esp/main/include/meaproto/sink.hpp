#pragma once
#ifndef __MEAPROTO_SINK_HPP__
#define __MEAPROTO_SINK_HPP__

#include "meaproto/types.hpp"

#include <span>

namespace meaproto
{
/** 应用侧接收接口：回调运行在 meaproto 任务上下文，只允许入队或拷贝 */
class Sink
{
  public:
    virtual ~Sink() = default;

    /** 对应发送端一次 send()：分片已重组、可靠消息已按序 */
    virtual void on_message(ChannelId channel, std::span<const u8> data) = 0;

    virtual void on_connected() = 0;
    virtual void on_disconnected(DisconnectReason reason) = 0;
};
} // namespace meaproto

#endif // __MEAPROTO_SINK_HPP__
