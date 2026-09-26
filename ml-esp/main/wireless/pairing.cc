#include "meguminlink/wireless/pairing.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/link.hpp"
#include <cstring>

using namespace mlink;

static constexpr char TAG[] = "mlink::pairing";

namespace
{
constexpr uint32_t PROBE_INTERVAL_MS = 500;
constexpr uint32_t REQUEST_RETRY_MS = 500;
constexpr int MAX_REQUEST_RETRIES = 5;
constexpr uint32_t WAITING_FOR_REQUEST_TIMEOUT_MS = 2000;
constexpr uint32_t ACCEPTED_GRACE_MS = 2000;
} // namespace

void PairingState::ensure_own_mac()
{
    if (m_mac_ready)
    {
        return;
    }
    m_mac_ready = esp_wifi_get_mac(WIFI_IF_STA, m_own_mac) == ESP_OK;
    if (!m_mac_ready && !m_mac_error_logged)
    {
        ESP_LOGE(TAG, "Failed to read own STA MAC, pairing suspended");
        m_mac_error_logged = true;
    }
}

esp_err_t PairingState::send_probe()
{
    Packet p;
    p.packet_type = PacketType::Probe;
    const esp_err_t ret = mlink::Application::get_instance().wireless_link().send(ESP_NOW_BROADCAST, p);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send probe: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t PairingState::send_request(const u8 *target)
{
    Packet p;
    p.packet_type = PacketType::PairingRequest;
    std::memcpy(p.payload.pairing_request.target_mac, target, ESP_NOW_ETH_ALEN);
    const esp_err_t ret = mlink::Application::get_instance().wireless_link().send(target, p);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send pairing request: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t PairingState::send_accepted(const u8 *target)
{
    Packet p;
    p.packet_type = PacketType::PairingAccepted;
    std::memcpy(p.payload.pairing_accepted.target_mac, target, ESP_NOW_ETH_ALEN);
    const esp_err_t ret = mlink::Application::get_instance().wireless_link().send(target, p);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send pairing accepted: %s", esp_err_to_name(ret));
    }
    return ret;
}

void PairingState::drop_peer()
{
    if (m_peer_mac[0] != 0 || m_peer_mac[1] != 0 || m_peer_mac[2] != 0 || m_peer_mac[3] != 0 || m_peer_mac[4] != 0 ||
        m_peer_mac[5] != 0)
    {
        mlink::Application::get_instance().wireless_link().peer_remove(m_peer_mac);
        std::memset(m_peer_mac, 0, ESP_NOW_ETH_ALEN);
    }
}

void PairingState::complete_pairing(Application &app, const u8 *peer)
{
    const esp_err_t ret = peer_mac_key().set(peer, ESP_NOW_ETH_ALEN);
    ESP_LOGI(TAG, "Pairing complete with " MACSTR " (nvs: %s)", MAC2STR(peer),
             ret == ESP_OK ? "ok" : esp_err_to_name(ret));
    drop_peer();
    // 长按进入的配对模式在此结束；未配对自动配对（非配对模式）不受影响。
    app.pairing_completed();
    // 配对是 Wireless 的子流程：完成后停留在 Wireless 模式，Application 检测到
    // NVS 中的 peer 记录即进入无线调试就绪状态，无需切换模式。
}

void PairingState::reset()
{
    drop_peer();
    m_state = State::Discovering;
    m_last_probe = 0;
    m_deadline = 0;
    m_retries = 0;
    m_send_failures = 0;
}

void PairingState::process(Application &app)
{
    ensure_own_mac();
    if (!m_mac_ready)
    {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    auto packets = app.take_received_packets();

    // 先处理超时（各状态独立计时，不依赖收包节奏）
    switch (m_state)
    {
    case State::Discovering: {
        if (now - m_last_probe >= pdMS_TO_TICKS(PROBE_INTERVAL_MS))
        {
            send_probe();
            m_last_probe = now;
        }
        break;
    }

    case State::WaitingForAccepted: {
        if (now >= m_deadline)
        {
            if (send_request(m_peer_mac) == ESP_OK)
            {
                if (++m_retries < MAX_REQUEST_RETRIES)
                {
                    m_deadline = now + pdMS_TO_TICKS(REQUEST_RETRY_MS);
                }
                else
                {
                    ESP_LOGW(TAG, "Request retries exhausted, re-discovering");
                    drop_peer();
                    m_state = State::Discovering;
                }
            }
            else
            {
                m_send_failures++;
                if (m_send_failures >= MAX_REQUEST_RETRIES)
                {
                    ESP_LOGW(TAG, "Too many send failures, re-discovering");
                    drop_peer();
                    m_state = State::Discovering;
                }
                else
                {
                    m_deadline = now + pdMS_TO_TICKS(REQUEST_RETRY_MS / 2);
                }
            }
        }
        break;
    }

    case State::WaitingForRequest: {
        // 持续广播 Probe：若本机（接收方）的 Probe 此前未被对端收到，
        // 对端将永远停留在 Discovering，导致配对卡死。因此接收等待期间继续广播。
        if (now - m_last_probe >= pdMS_TO_TICKS(PROBE_INTERVAL_MS))
        {
            send_probe();
            m_last_probe = now;
        }
        if (now >= m_deadline)
        {
            ESP_LOGW(TAG, "Timed out waiting for request, re-discovering");
            drop_peer();
            m_state = State::Discovering;
        }
        break;
    }

    case State::AcceptedGrace: {
        if (now >= m_deadline)
        {
            complete_pairing(app, m_peer_mac);
            return;
        }
        break;
    }
    }

    // 再处理收包；状态可能在本轮遍历中迁移，每次迭代都读取最新状态
    for (const auto &r : packets)
    {
        switch (m_state)
        {
        case State::Discovering: {
            // 对端可能先于我收到 Probe 并抢先发起请求：此时直接接受，
            // 不必等本机也收到 Probe 再进入 WaitingForRequest
            if (r.packet.packet_type == PacketType::PairingRequest &&
                std::memcmp(r.src_mac, m_own_mac, ESP_NOW_ETH_ALEN) < 0)
            {
                std::memcpy(m_peer_mac, r.src_mac, ESP_NOW_ETH_ALEN);
                mlink::Application::get_instance().wireless_link().peer_add(m_peer_mac);
                send_accepted(m_peer_mac);
                m_state = State::AcceptedGrace;
                m_deadline = now + pdMS_TO_TICKS(ACCEPTED_GRACE_MS);
                ESP_LOGI(TAG, "Received early pairing request, accepting");
                break;
            }
            if (r.packet.packet_type != PacketType::Probe)
            {
                break;
            }
            {
                const int cmp = std::memcmp(r.src_mac, m_own_mac, ESP_NOW_ETH_ALEN);
                if (cmp < 0)
                {
                    // 对端是发起方，本机转为接收方
                    std::memcpy(m_peer_mac, r.src_mac, ESP_NOW_ETH_ALEN);
                    mlink::Application::get_instance().wireless_link().peer_add(m_peer_mac);
                    m_state = State::WaitingForRequest;
                    m_deadline = now + pdMS_TO_TICKS(WAITING_FOR_REQUEST_TIMEOUT_MS);
                    ESP_LOGI(TAG, "Peer " MACSTR " has smaller MAC, waiting for request", MAC2STR(r.src_mac));
                }
                else if (cmp > 0)
                {
                    // 本机是发起方，立即发出请求
                    std::memcpy(m_peer_mac, r.src_mac, ESP_NOW_ETH_ALEN);
                    mlink::Application::get_instance().wireless_link().peer_add(m_peer_mac);
                    m_state = State::WaitingForAccepted;
                    m_retries = 0;
                    m_deadline = now + pdMS_TO_TICKS(REQUEST_RETRY_MS);
                    ESP_LOGI(TAG, "Peer " MACSTR " has larger MAC, sending request", MAC2STR(r.src_mac));
                    send_request(m_peer_mac);
                }
            }
            break;
        }

        case State::WaitingForAccepted: {
            if (r.packet.packet_type == PacketType::PairingAccepted &&
                std::memcmp(r.src_mac, m_peer_mac, ESP_NOW_ETH_ALEN) == 0)
            {
                complete_pairing(app, m_peer_mac);
                return;
            }
            break;
        }

        case State::WaitingForRequest: {
            if (r.packet.packet_type == PacketType::PairingRequest &&
                std::memcmp(r.src_mac, m_peer_mac, ESP_NOW_ETH_ALEN) == 0)
            {
                ESP_LOGI(TAG, "Received pairing request, accepting");
                send_accepted(m_peer_mac);
                m_state = State::AcceptedGrace;
                m_deadline = now + pdMS_TO_TICKS(ACCEPTED_GRACE_MS);
            }
            break;
        }

        case State::AcceptedGrace: {
            // 发起方可能因 Accepted 丢包而重发请求，宽限期内继续回应
            if (r.packet.packet_type == PacketType::PairingRequest &&
                std::memcmp(r.src_mac, m_peer_mac, ESP_NOW_ETH_ALEN) == 0)
            {
                send_accepted(m_peer_mac);
                m_deadline = now + pdMS_TO_TICKS(ACCEPTED_GRACE_MS);
            }
            break;
        }
        }
    }
}
