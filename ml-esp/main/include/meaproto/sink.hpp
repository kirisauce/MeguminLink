#pragma once
#ifndef __MEAPROTO_SINK_HPP__
#define __MEAPROTO_SINK_HPP__

#include "meaproto/types.hpp"

#include <span>

namespace meaproto
{
/**
 * 应用侧接收接口：回调运行在 meaproto 任务上下文，只允许入队或拷贝。
 *
 * 回调期间协议锁由任务持有，因此回调里不得调用 Daemon::send/can_send/ping/
 * set_reliability——那会在同一上下文里自锁。
 */
class Sink
{
  public:
    virtual ~Sink() = default;

    /**
     * 对应发送端一次 send()：分片已重组、可靠消息已按序。
     *
     * 返回 false 表示本端落口暂时吃不下（队列已满），Core 不推进接收序号、不确认
     * 该数据报，等对端重传补齐。
     */
    virtual bool on_message(ChannelId channel, std::span<const u8> data) = 0;

    /**
     * 落口是否还有余量：false 时 Core 照常收下数据但暂缓确认，让对端窗口停住，
     * 等本端把落口写出去。
     */
    virtual bool rx_ready(ChannelId channel) const noexcept
    {
        (void)channel;
        return true;
    }

    virtual void on_connected() = 0;
    virtual void on_disconnected(DisconnectReason reason) = 0;
};
} // namespace meaproto

#endif // __MEAPROTO_SINK_HPP__
