#ifndef __MEGUMINLINK_PAIRING_HPP__
#define __MEGUMINLINK_PAIRING_HPP__

#include "esp_now.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "meguminlink/common.hpp"

namespace mlink
{
class Application;

/**
 * 配对子状态机（Wireless 模式未配对时的建链流程，见 Application::run）
 *
 * Discovering      广播 Probe 发现对端；收到 Probe 后按 MAC 仲裁：
 *                  - 对端 MAC 更小 → 对端发起，本机进入 WaitingForRequest
 *                  - 对端 MAC 更大 → 本机发起，进入 WaitingForAccepted
 * WaitingForAccepted   发起方：等待 PairingAccepted，超时重发 PairingRequest
 * WaitingForRequest    接收方：等待 PairingRequest，超时回 Discovering
 * AcceptedGrace        接收方：已回 PairingAccepted，宽限期内继续回应重发
 */
class PairingState
{
  public:
    /** 重置内部状态（重新进入配对流程时调用） */
    void reset();

    /** 驱动状态机推进（由 Application 在 Wireless 模式未配对时周期调用） */
    void process(Application &app);

  private:
    enum class State
    {
        Discovering,
        WaitingForAccepted,
        WaitingForRequest,
        AcceptedGrace,
    };

    State m_state = State::Discovering;
    u8 m_own_mac[ESP_NOW_ETH_ALEN] = {};
    u8 m_peer_mac[ESP_NOW_ETH_ALEN] = {};
    TickType_t m_last_probe = 0;
    TickType_t m_deadline = 0;
    int m_retries = 0;
    int m_send_failures = 0;
    bool m_mac_ready = false;
    bool m_mac_error_logged = false;

    void ensure_own_mac();
    esp_err_t send_probe();
    esp_err_t send_request(const u8 *target);
    esp_err_t send_accepted(const u8 *target);
    void drop_peer();
    void complete_pairing(Application &app, const u8 *peer);
};
} // namespace mlink

#endif
