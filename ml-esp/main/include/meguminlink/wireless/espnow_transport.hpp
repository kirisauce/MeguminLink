#pragma once
#ifndef __MEGUMINLINK_WIRELESS_ESPNOW_TRANSPORT_HPP__
#define __MEGUMINLINK_WIRELESS_ESPNOW_TRANSPORT_HPP__

#include "esp_err.h"
#include "esp_now.h"
#include "esp_wifi.h" // IWYU pragma: keep
#include "meaproto/transport.hpp"
#include "meguminlink/common.hpp"

#include <freertos/semphr.h>

#include <span>

namespace mlink
{
/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** ESP-NOW 固定信道 */
inline constexpr u8 ESPNOW_CHANNEL = 10;

/**
 * WiFi / ESP-NOW 的所有权：初始化、peer 绑定、发送、收帧分流。
 *
 * 满足 meaproto::Transport，只认"一帧字节"。首字节高 4 位等于 meaproto 版本、
 * 且来自已绑定 peer 的帧交给协议层；其余（配对包等非协议帧）交给非协议回调。
 *
 * WiFi 与 ESP-NOW 是全局资源，全进程只有一个实例。
 */
class EspNowTransport
{
  public:
    /** 非 meaproto 帧（配对包等）的落点 */
    using NonMeaprotoFrameFn = void (*)(void *ctx, const u8 *src_mac, const u8 *data, usize len);

    /** 收帧观测：把空口字节与 RSSI 交给射频统计 */
    using RxObserverFn = void (*)(void *ctx, u16 bytes, i8 rssi);

    /** 发送结果：peer 未知时为 nullptr */
    using TxResultFn = void (*)(void *ctx, const u8 *peer_mac, bool ok);

    /** 唯一实例 */
    static EspNowTransport &instance() noexcept;

    EspNowTransport(const EspNowTransport &) = delete;
    EspNowTransport &operator=(const EspNowTransport &) = delete;

    /** 初始化 WiFi 与 ESP-NOW（幂等） */
    esp_err_t init();

    /** 绑定唯一 peer：登记进 peer list，之后 send() 都发往它 */
    esp_err_t bind(const u8 *peer_mac);
    void unbind();

    // meaproto::Transport
    /** ESP-NOW v2 的载荷上限 */
    static constexpr usize MTU = ESP_NOW_MAX_DATA_LEN_V2;

    bool send(std::span<const u8> data) noexcept;
    void set_receive_handler(meaproto::ReceiveFn callback, void *ctx) noexcept;

    /** 发给任意已登记的 peer（配对帧与广播） */
    esp_err_t send_to(const u8 *peer_mac, std::span<const u8> data) noexcept;

    void set_non_meaproto_frame_callback(NonMeaprotoFrameFn callback, void *ctx) noexcept;
    void set_rx_observer_callback(RxObserverFn callback, void *ctx) noexcept;
    void set_tx_result_callback(TxResultFn callback, void *ctx) noexcept;

    esp_err_t peer_add(const u8 *peer_mac);
    esp_err_t peer_remove(const u8 *peer_mac);

    /** 对端级固定速率（广播地址跳过） */
    esp_err_t set_peer_rate(const u8 *peer_mac, u8 rate);

    /** 该速率档位是否在 ESP-NOW 可配置范围内 */
    static bool rate_supported(u8 rate) noexcept;

    u32 tx_frames() const noexcept;
    u32 tx_failures() const noexcept;

  private:
    EspNowTransport() = default;

    static void recv_cb(const esp_now_recv_info_t *info, const u8 *data, int len);
    static void send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status);

    esp_err_t transmit(const u8 *peer_mac, std::span<const u8> data) noexcept;

    // ---- 生命周期与 peer ----
    bool m_initialized = false;
    bool m_bound = false;
    u8 m_peer_mac[ESP_NOW_ETH_ALEN] = {};

    // ---- 回调出口（回调与上下文成对设置）----
    meaproto::ReceiveFn m_receive_callback = nullptr; /**< meaproto 协议帧 */
    void *m_receive_ctx = nullptr;
    NonMeaprotoFrameFn m_non_meaproto_frame_callback = nullptr; /**< 非协议帧（配对等） */
    void *m_non_meaproto_frame_ctx = nullptr;
    RxObserverFn m_rx_observer_callback = nullptr; /**< 每帧的字节数与 RSSI */
    void *m_rx_observer_ctx = nullptr;
    TxResultFn m_tx_result_callback = nullptr; /**< 每帧的发送结果 */
    void *m_tx_result_ctx = nullptr;

    // ---- 发送 ----
    SemaphoreHandle_t m_tx_mutex = nullptr; /**< esp_now_send 串行化 */
    u32 m_tx_frames = 0;
    u32 m_tx_failures = 0;
    u32 m_next_fail_log_ms = 0; /**< 失败日志的限速时刻 */
};
} // namespace mlink

#endif // __MEGUMINLINK_WIRELESS_ESPNOW_TRANSPORT_HPP__
