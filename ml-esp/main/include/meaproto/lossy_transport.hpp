#pragma once
#ifndef __MEAPROTO_LOSSY_TRANSPORT_HPP__
#define __MEAPROTO_LOSSY_TRANSPORT_HPP__

#include "meaproto/transport.hpp"

#include <atomic>
#include <span>

namespace meaproto
{
/** 注入判定的观测回调：每帧一次，dropped 表示该帧被丢弃 */
using LossObserver = void (*)(void *ctx, bool dropped);

/**
 * 发送侧的丢包注入：包住任意 Transport，按千分比丢弃出向帧。
 *
 * 只在 send() 上丢，接收与绑定语义全部转交内层：被丢的帧对协议层与真实丢包不可
 * 区分，重传、去重与超时路径因此都走真实链路的那一套。
 */
template <Transport Inner> class LossyTransport
{
  public:
    static constexpr usize MTU = Inner::MTU;

    LossyTransport(Inner &inner, LossObserver observer, void *ctx) noexcept
        : m_inner(inner), m_observer(observer), m_ctx(ctx)
    {
    }

    LossyTransport(const LossyTransport &) = delete;
    LossyTransport &operator=(const LossyTransport &) = delete;

    /** 注入丢包率（千分比）：seed 用于换档重播种，0 按 1 处理 */
    void set_permille(u8 permille, u32 seed) noexcept
    {
        m_rng = (seed != 0) ? seed : 1; // xorshift 的 0 是不动点
        m_permille.store(permille, std::memory_order_relaxed);
    }

    u8 permille() const noexcept
    {
        return m_permille.load(std::memory_order_relaxed);
    }

    // meaproto::Transport
    bool send(std::span<const u8> data) noexcept
    {
        const u8 permille = m_permille.load(std::memory_order_relaxed);
        const bool drop = (permille != 0) && next_drop(permille);

        if (m_observer != nullptr)
        {
            m_observer(m_ctx, drop);
        }
        return drop || m_inner.send(data);
    }

    void set_receive_handler(ReceiveFn callback, void *ctx) noexcept
    {
        m_inner.set_receive_handler(callback, ctx);
    }

  private:
    /** 一次概率判定；状态只在发送路径（协议任务）推进 */
    bool next_drop(u8 permille) noexcept
    {
        u32 x = m_rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_rng = x;
        return (x % 1000u) < permille;
    }

    Inner &m_inner;
    LossObserver m_observer = nullptr;
    void *m_ctx = nullptr;
    std::atomic<u8> m_permille{0};
    u32 m_rng = 1;
};
} // namespace meaproto

#endif
