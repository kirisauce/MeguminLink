#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/wireless_link.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "meguminlink/app.hpp"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp" // dap_executor_run（无线侧执行接线）
#include "meguminlink/nvs_flash.hpp"
#include <cstring>

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 usb.cc 一致）。
#include "DAP.h"
#include "DAP_config.h" // DAP_PACKET_SIZE（命令长度上限/请求缓冲）

using namespace mlink;

static constexpr char TAG[] = "mlink::wireless_link";

static constexpr u8 CHANNEL = 10;

namespace
{
/** 无线发送帧缓冲（栈上）：帧 ≤ kDapFrameHeaderSize + kMaxDapPayloadSize = 67B */
constexpr size_t kMaxFrameSize = kDapFrameHeaderSize + kMaxDapPayloadSize;
} // namespace

static void link_recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len)
{
    if (recv_info == nullptr || recv_info->src_addr == nullptr)
    {
        return;
    }

    ReceivedPacket rp = {};
    std::memcpy(rp.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    if (rp.packet.deserialize(data, data_len, rp.storage, sizeof(rp.storage)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to deserialize %d-byte frame from " MACSTR, data_len, MAC2STR(rp.src_mac));
        return;
    }

    Application::get_instance().push_received_packet(rp);
}

static void link_send_cb(const wifi_tx_info_t *send_info, esp_now_send_status_t send_status)
{
    if (send_status != ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGW(TAG, "ESP-NOW send failed");
    }
}

static esp_err_t ensure_broadcast_peer()
{
    if (esp_now_is_peer_exist(ESP_NOW_BROADCAST))
    {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, ESP_NOW_BROADCAST, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register broadcast peer: %s", esp_err_to_name(ret));
    }
    return ret;
}

static inline esp_err_t wireless_link_register()
{
    esp_err_t ret;

    ret = esp_now_register_recv_cb(link_recv_cb);
    __check("Failed to register ESP-NOW receive callback: %s", esp_err_to_name(ret));

    ret = esp_now_register_send_cb(link_send_cb);
    __check("Failed to register ESP-NOW send callback: %s", esp_err_to_name(ret));

    return ESP_OK;

err:
    return ret;
}

esp_err_t mlink::wireless_link_init()
{
    static bool initialized = false;
    if (initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;
    ret = mlink::nvs_flash_init();
    __check("mlink::nvs_flash_init() failed");

    ret = esp_netif_init();
    __check("esp_netif_init() failed");
    ret = esp_event_loop_create_default();
    __check("esp_event_loop_create_default() failed");

    {
        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&wifi_init_cfg);
        __check("esp_wifi_init() failed");
        ESP_LOGI(TAG, "WiFi initialized");

        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        __check("esp_wifi_set_storage() failed");
    }

    // ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    // ESP_LOGI(TAG, "WiFi promiscuous mode was enabled");

    {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        __check("esp_wifi_set_mode() failed");

        wifi_config_t wifi_cfg = {};
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        __check("esp_wifi_set_config() failed");

        ret = esp_wifi_start();
        __check("esp_wifi_start() failed");

        ret = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
        __check("esp_wifi_set_channel() failed");
    }

    ret = esp_now_init();
    __check("esp_now_init() failed");

    ret = ensure_broadcast_peer();
    __check("ensure_broadcast_peer() failed: %s", esp_err_to_name(ret));

    ret = wireless_link_register();
    if (ret != ESP_OK)
        goto err;

    initialized = true;
    return ESP_OK;

err:
    return ret;
}

esp_err_t mlink::wireless_link_peer_add(const u8 *peer_mac)
{
    if (esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add peer " MACSTR ": %s", MAC2STR(peer_mac), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mlink::wireless_link_peer_remove(const u8 *peer_mac)
{
    if (!esp_now_is_peer_exist(peer_mac))
    {
        return ESP_OK;
    }

    const esp_err_t ret = esp_now_del_peer(peer_mac);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to remove peer " MACSTR ": %s", MAC2STR(peer_mac), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mlink::wireless_link_send(const u8 *dest_mac, const Packet &packet)
{
    // 栈上缓冲（Packet 小巧：数据段 ≤64B，帧 ≤67B）
    u8 buffer[kMaxFrameSize];
    const esp_err_t ret = packet.serialize(buffer, sizeof(buffer));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to serialize packet: %s", esp_err_to_name(ret));
        return ret;
    }

    return esp_now_send(dest_mac, buffer, packet.serialized_size());
}

esp_err_t mlink::wireless_link_send_dap_command(const u8 *dest_mac, const u8 *data, size_t len)
{
    if (len > kMaxDapPayloadSize)
    {
        ESP_LOGE(TAG, "DAP command too large (%u bytes, max %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(kMaxDapPayloadSize));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：data 指向调用方命令缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::DapCommand;
    auto &content = packet.payload.dap_command;
    content.len = static_cast<u16>(len);
    content.data = data;
    return wireless_link_send(dest_mac, packet);
}

esp_err_t mlink::wireless_link_send_dap_response(const u8 *dest_mac, const u8 *data, size_t len)
{
    if (len > kMaxDapPayloadSize)
    {
        ESP_LOGE(TAG, "DAP response too large (%u bytes, max %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(kMaxDapPayloadSize));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：data 指向调用方响应缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::DapResponse;
    auto &content = packet.payload.dap_response;
    content.len = static_cast<u16>(len);
    content.data = data;
    return wireless_link_send(dest_mac, packet);
}

esp_err_t mlink::wireless_link_forward_dap_command(const u8 *cmd, size_t len, u8 *resp, size_t resp_cap, u16 *resp_len)
{
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (!mlink::peer_mac_load(peer_mac))
    {
        ESP_LOGW(TAG, "Wireless: no paired peer, %u-byte DAP request dropped", static_cast<unsigned>(len));
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret;
    ret = wireless_link_send_dap_command(peer_mac, cmd, len);
    __check("Wireless: failed to forward %u-byte DAP request: %s", static_cast<unsigned>(len), esp_err_to_name(ret));
    {
        // 阻塞等待接收端响应（一次一个在途请求，与 CMSIS-DAP v2 同步请求/响应
        // 语义一致；等待期间不处理下一个请求）。
        u16 rlen = 0;
        if (!Application::get_instance().wait_wireless_response(resp, &rlen,
                                                                CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "Wireless: no response within %u ms, %u-byte request dropped",
                     CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS, static_cast<unsigned>(len));
            return ESP_ERR_TIMEOUT;
        }

        if (rlen > resp_cap)
        {
            // 防御性截断（正常不会触发：接收端只发送 ≤ kMaxDapPayloadSize 的响应）
            ESP_LOGW(TAG, "Wireless response truncated (%u -> %u)", rlen, static_cast<unsigned>(resp_cap));
            rlen = static_cast<u16>(resp_cap);
        }
        *resp_len = rlen;
    }
    return ESP_OK;

err:
    return ret;
}

#define __check_size(parser, expected_size)                                                                            \
    if (parser.remaining() < expected_size)                                                                            \
    {                                                                                                                  \
        return ESP_ERR_INVALID_SIZE;                                                                                   \
    }

Packet::Packet()
{
}

size_t Packet::serialized_size() const
{
    switch (packet_type)
    {
    case PacketType::Probe: {
        return 1;
    }
    case PacketType::PairingRequest: {
        return 1 + ESP_NOW_ETH_ALEN;
    }
    case PacketType::PairingAccepted: {
        return 1 + ESP_NOW_ETH_ALEN;
    }
    case PacketType::DapCommand:
    case PacketType::DapResponse: {
        const u16 len = (packet_type == PacketType::DapCommand) ? payload.dap_command.len : payload.dap_response.len;
        return kDapFrameHeaderSize + len;
    }
    default: {
        // 与 serialize() 的 ESP_ERR_NOT_SUPPORTED 对齐：未知类型按非法处理
        return 0;
    }
    }
}

esp_err_t Packet::deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap)
{
    BinDeserializer p(data, data + len);

    __check_size(p, 1);
    packet_type = static_cast<PacketType>(p.read_u8());

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        __check_size(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        __check_size(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        // 数据段拷入调用方存储，data 指向该存储（见 ReceivedPacket::storage）
        auto &content = payload.dap_command;
        __check_size(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        __check_size(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        __check_size(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        __check_size(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    default: {
        break;
    }
    }

    return ESP_OK;
}

esp_err_t Packet::serialize(u8 *buffer, size_t len) const
{
    BinSerializer s(buffer, len);
    __check_size(s, 1);
    s.write_u8(static_cast<u8>(packet_type));

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        __check_size(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        __check_size(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        auto &content = payload.dap_command;
        if (content.data == nullptr || content.len > kMaxDapPayloadSize)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        __check_size(s, 2);
        s.write_u16(content.len);
        __check_size(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        if (content.data == nullptr || content.len > kMaxDapPayloadSize)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        __check_size(s, 2);
        s.write_u16(content.len);
        __check_size(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef __check_size

// ---- 无线侧 DAP 执行接线 ----
// 与 usb.cc 的 dap_task 对称：无线传输层负责把收到的 DapCommand 交给公共
// 执行器（dap_executor_run）执行，并把结果以 DapResponse 帧回发给来源。
// 执行循环、DAP_Setup 与命令日志均在 dap_executor.cc，这里只有队列、
// 任务与输入/输出回调；状态全在 WirelessDapExecutor 对象内（无全局变量）。

namespace
{
constexpr size_t QUEUE_DEPTH = 8;

/** 一条待执行的 DAP 命令：命令来源（响应回发）+ 原样命令字节流 */
struct ExecuteRequest
{
    u8 src_mac[ESP_NOW_ETH_ALEN];
    u8 data[DAP_PACKET_SIZE];
    u16 len;
};
} // namespace

bool WirelessDapExecutor::cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto *executor = static_cast<WirelessDapExecutor *>(arg);

    ExecuteRequest req;
    if (xQueueReceive(executor->m_queue, &req, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    if (req.len > buf_cap)
    {
        // 防御：命令不得超过执行器请求缓冲（DAP_PACKET_SIZE）
        ESP_LOGW(TAG, "DAP command too large (%u bytes, cap %u), dropped", static_cast<unsigned>(req.len),
                 static_cast<unsigned>(buf_cap));
        return false;
    }
    std::memcpy(buf, req.data, req.len);
    *len = req.len;
    std::memcpy(executor->m_src_mac, req.src_mac, ESP_NOW_ETH_ALEN);
    return true;
}

void WirelessDapExecutor::resp_sink(const u8 *data, u16 len, void *arg)
{
    auto *executor = static_cast<WirelessDapExecutor *>(arg);
    const esp_err_t ret = wireless_link_send_dap_response(executor->m_src_mac, data, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send DAP response: %s", esp_err_to_name(ret));
    }
}

void WirelessDapExecutor::task(void *arg)
{
    auto *executor = static_cast<WirelessDapExecutor *>(arg);
    // 复用公共执行器：取命令（无线队列）→ 执行 → 响应回发；
    // 命令/响应缓冲用执行器成员（不占任务栈，见 dap_executor.hpp）
    dap_executor_run(WirelessDapExecutor::cmd_src, WirelessDapExecutor::resp_sink, arg, executor->m_request,
                     sizeof(executor->m_request), executor->m_response, sizeof(executor->m_response));
}

esp_err_t WirelessDapExecutor::init()
{
    if (m_task != nullptr)
    {
        return ESP_OK; // 幂等
    }

    m_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ExecuteRequest));
    if (m_queue == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create DAP command queue");
        return ESP_ERR_NO_MEM;
    }

    // 优先级 24（> esp_wifi 任务 23）：单核（ESP32-C3）上保证 SWD 位碰撞
    // 执行期间不被任务抢占（抢占会破坏 SWCLK 时序，见 usb.cc 注释）；
    // 任务平时阻塞在队列接收上，不占用 CPU，短暂最高优先级无害。
    // 栈 4096 字节：DAP 执行调用链（DAP_ExecuteCommand 等）在栈上运行。
    if (xTaskCreate(WirelessDapExecutor::task, "dap-wireless", 4096, this, 24, &m_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create wireless DAP executor task");
        vQueueDelete(m_queue);
        m_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t WirelessDapExecutor::execute(const u8 *src_mac, const u8 *data, size_t len)
{
    if (m_queue == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > DAP_PACKET_SIZE)
    {
        ESP_LOGW(TAG, "DAP command too large (%u bytes, max %u), dropped", static_cast<unsigned>(len),
                 static_cast<unsigned>(DAP_PACKET_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    ExecuteRequest req = {};
    std::memcpy(req.src_mac, src_mac, ESP_NOW_ETH_ALEN);
    req.len = static_cast<u16>(len);
    std::memcpy(req.data, data, len);

    if (xQueueSend(m_queue, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "DAP command queue full, command dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
