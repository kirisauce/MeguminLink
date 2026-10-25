#pragma once
#ifndef __MEAPROTO_CONFIG_HPP__
#define __MEAPROTO_CONFIG_HPP__

#include "meaproto/types.hpp"

namespace meaproto
{
/** 通道表条目上限（编译期固定） */
inline constexpr usize MAX_CHANNELS = 8;

/** 每通道发送窗口槽上限（编译期固定） */
inline constexpr usize MAX_WINDOW = 16;

/** 协议运行参数；尺寸类字段在构造 Daemon 时生效 */
struct Config
{
    u16 datagram_window = 16; /**< 每通道默认未确认窗口 */
    u16 rto_initial_ms = 100;
    u16 rto_max_ms = 1600;
    u16 keepalive_interval_ms = 1000;
    u16 connect_timeout_ms = 5000;
    u16 connect_retry_ms = 250;
    u8 connect_max_retries = 8;
    u8 send_quota_per_tick = 8; /**< 每 tick 最多发几帧 */
    u16 ack_delay_ms = 2;       /**< 延迟确认 */
    u16 fragment_timeout_ms = 5000;
    u16 max_message_size = 8192;
    u8 rx_queue_depth = 8;
    u8 max_reassembly_slots = 2;
};
} // namespace meaproto

#endif // __MEAPROTO_CONFIG_HPP__
