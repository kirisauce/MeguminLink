#pragma once
#ifndef __MEAPROTO_TYPES_HPP__
#define __MEAPROTO_TYPES_HPP__

#include "meguminlink/common.hpp"

namespace meaproto
{
/** 通道标识：占 4 位，0 保留给协议内部 */
struct ChannelId
{
    u8 value = 0;

    constexpr explicit ChannelId(u8 v = 0) noexcept : value(v)
    {
    }

    friend constexpr bool operator==(ChannelId, ChannelId) noexcept = default;
};

enum class Reliability : u8
{
    Unreliable,
    ReliableOrdered,
};

/** 一条通道的配置：通道表由应用在构造 Daemon 时提供 */
struct ChannelSpec
{
    ChannelId id{};                                       /**< 通道标识，0 保留给协议内部 */
    Reliability reliability = Reliability::ReliableOrdered; /**< 该通道的可靠性策略 */
    u16 max_payload = 0;                                  /**< 单条消息的载荷上限，不含消息头 */
    u8 queue_depth = 0;                                   /**< 发送队列可排队的消息条数 */
    u8 priority = 0;                                      /**< 发送优先级，数值小的先发 */
    u8 reorder_depth = 0;                                 /**< 接收重排缓冲可暂存的消息条数，0 时取 queue_depth */
    u8 window = 0;                                        /**< 未确认帧上限，0 时取 Config::datagram_window */
};

enum class SendResult : u8
{
    Ok,
    NotConnected,
    InvalidChannel,
    TooLarge,
    Busy,
    InvalidState,
};

enum class DisconnectReason : u8
{
    Requested,
    Timeout,
    Rejected,
    TransportError,
};

struct Stats
{
    u32 datagrams_sent = 0;
    u32 datagrams_retransmitted = 0;
    u32 datagrams_acked = 0;
    u32 datagrams_in_flight = 0;
    u32 messages_delivered = 0;
    u32 messages_dropped = 0;
    u32 duplicate_datagrams = 0;
    u32 out_of_window = 0; /**< 序号超出 ACK 位图覆盖范围 */
    u32 tx_queue_dropped = 0;
    u32 rx_queue_dropped = 0;
    u32 reorder_overflow = 0; /**< 重排缓冲满导致帧被丢弃 */
    u32 fragments_reassembled = 0;
    u32 reassembly_timeouts = 0;
    u32 rtt_ms = 0;
    u32 rto_ms = 0;
    u16 peer_epoch = 0;
    bool connected = false;
};
} // namespace meaproto

#endif // __MEAPROTO_TYPES_HPP__
