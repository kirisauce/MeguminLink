#pragma once
#ifndef __MEAPROTO_REASSEMBLY_HPP__
#define __MEAPROTO_REASSEMBLY_HPP__

#include "meaproto/config.hpp"
#include "meaproto/types.hpp"
#include "meaproto/wire.hpp"

#include <cstddef>
#include <span>

namespace meaproto
{
/**
 * 分片重组槽池。
 *
 * 只负责「收片 → 拼消息 → 回调交付」，不碰通道序号与重传：完成的消息交给
 * 回调，回调拒绝时保留槽等末片重传；存储由调用方切一块交进来。
 */
class Reassembler
{
  public:
    /** 收齐一条消息时回调；返回 false 表示暂不能交付，本帧不要确认 */
    using Deliver = bool (*)(void *ctx, ChannelId channel, u16 ordering, const u8 *data, usize len) noexcept;

    /** 有效槽数：min(config.max_reassembly_slots, MAX_REASSEMBLY_SLOTS) */
    static u16 capacity(const Config &config) noexcept;

    /** 单条消息的片数上限 */
    static u16 max_fragments(const Config &config, u16 mtu) noexcept;

    /** 重组区总字节数 */
    static usize bytes(const Config &config, u16 mtu) noexcept;

    /** 切分存储，返回占用的字节数；容量不足时按 0 槽处理 */
    usize init(std::span<u8> storage, const Config &config, u16 mtu) noexcept;

    /**
     * 收一片。
     *
     * @param chunk    本端片载荷上限，用于校验长度与推算偏移
     * @param deliver  收齐后的交付回调
     * @return true = 该帧可以确认；false = 不要确认（槽满或交付被打回）
     */
    bool push(ChannelId channel, const MessageHeader &header, const u8 *payload, u16 chunk, Deliver deliver,
              void *ctx) noexcept;

    /** 清空全部槽 */
    void clear() noexcept;

    /** 清空某通道的槽 */
    void clear(ChannelId channel) noexcept;

    /** 已重组完成的消息数 */
    u32 completed() const noexcept;

    /** 非法片、槽满与交付打回丢弃的片数 */
    u32 rejected() const noexcept;

  private:
    /** 一条分片消息收齐前的暂存 */
    struct Slot
    {
        bool used = false;
        u8 channel = 0;
        u16 split_id = 0;
        u16 split_count = 0;
        u16 received_count = 0;
        u16 total_len = 0;
        u16 ordering = 0;
        u8 *data = nullptr;
        u8 *received = nullptr; /**< split_index 位图 */
    };

    Slot *find(ChannelId channel, u16 split_id) noexcept;
    Slot *take() noexcept;
    void release(Slot &slot) noexcept;

    /** 把收齐的槽交给回调；交付成功才释放槽 */
    bool flush_slot(Slot &slot, Deliver deliver, void *ctx) noexcept;

    Slot m_slots[MAX_REASSEMBLY_SLOTS];
    u8 m_count = 0;         /**< 有效槽数 */
    u8 m_used = 0;          /**< 已占用槽数 */
    usize m_data_bytes = 0; /**< 每槽数据区容量 */
    usize m_bits_bytes = 0; /**< 每槽位图字节数 */
    usize m_stride = 0;     /**< 每槽字节数 */
    u32 m_completed = 0;
    u32 m_rejected = 0;
};
} // namespace meaproto

#endif // __MEAPROTO_REASSEMBLY_HPP__
