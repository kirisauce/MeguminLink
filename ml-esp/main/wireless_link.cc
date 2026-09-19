#include "meguminlink/wireless_link.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp" // dap_executor_run（无线侧执行接线）
#include "meguminlink/nvs_flash.hpp"

#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 usb.cc 一致）。
// DAP_config.h 提供 DELAY_SLOW_CYCLES/PIN_DELAY_SLOW 等平台实现，
// DAP.h 仅在未定义时提供默认值（#ifndef 保护）；顺序反了会触发宏重定义。
// clang-format 的字典序排序会把它们排反，此处保持顺序：
// clang-format off
#include "DAP_config.h" // DAP_PACKET_SIZE（命令长度上限/请求缓冲）
#include "DAP.h" // IWYU pragma: keep
// clang-format on

using namespace mlink;

static constexpr char TAG[] = "mlink::wireless_link";

namespace
{
/** 无线侧 DAP 执行器：命令队列深度 */
constexpr size_t DAP_QUEUE_DEPTH = 8;

/** 一条待执行的 DAP 命令：命令来源（响应回发）+ 原样命令字节流 */
struct DapExecuteRequest
{
    u8 src_mac[ESP_NOW_ETH_ALEN];
    u8 data[MAX_DAP_PAYLOAD_SIZE];
    u16 len;
};

/**
 * 由速率枚举值推导 PHY 模式。ESP-NOW 的速率配置要求 phymode 与 rate 一致，
 * 而 phymode 完全由档位所在段决定（CCK 属 11b、OFDM 属 11g、MCS 属 HT20），
 * 因此按段推导而不是维护一张与枚举同步的手写表。
 * @return false = 该档位不在 ESP-NOW 可配置的范围内
 */
bool phymode_for_rate(u8 rate, wifi_phy_mode_t *mode)
{
    if (rate <= WIFI_PHY_RATE_11M_S)
    {
        // 0x00-0x07：CCK（短/长前导）。0x04 是 wifi_phy_rate_t 里的空洞
        if (rate == 0x04)
        {
            return false;
        }
        *mode = WIFI_PHY_MODE_11B;
        return true;
    }
    if (rate <= WIFI_PHY_RATE_9M)
    {
        *mode = WIFI_PHY_MODE_11G; // 0x08-0x0F：OFDM 6-54M
        return true;
    }
    if (rate <= WIFI_PHY_RATE_MCS7_SGI)
    {
        *mode = WIFI_PHY_MODE_HT20; // 0x10-0x1F：HT20 MCS0-7（LGI/SGI）
        return true;
    }
    return false; // 超出 11b/11g/HT20：本链路不做支持
}
} // namespace

bool mlink::wifi_rate_supported(u8 rate)
{
    wifi_phy_mode_t mode;
    return phymode_for_rate(rate, &mode);
}

// ---- ESP-NOW 回调（esp_now 要求 C 函数指针，无 arg 参数；经 Application 访问） ----

void WirelessLink::recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len)
{
    if (recv_info == nullptr || recv_info->src_addr == nullptr)
    {
        return;
    }

    // 反序列化存储随无线对象持有：数据段可达 1467B，放栈上会吃掉 ESP-NOW
    // 回调所在任务的栈。
    WirelessLink &link = Application::get_instance().wireless_link();

    ReceivedPacket rp = {};
    std::memcpy(rp.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    // 链路层统计：收到的每一帧都计（含反序列化失败的畸形帧——它们同样
    // 占用了空口与 ESP-NOW 任务），RSSI 随帧更新
    if (recv_info->rx_ctrl != nullptr)
    {
        Application::get_instance().config_channel().add_wireless_rx(static_cast<u16>(data_len),
                                                                     recv_info->rx_ctrl->rssi);
    }

    if (rp.packet.deserialize(data, data_len, link.m_rx_storage, sizeof(link.m_rx_storage)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to deserialize %d-byte frame from " MACSTR, data_len, MAC2STR(rp.src_mac));
        return;
    }

    // DAP 数据帧直通投递（不经主循环）。A/B 实测（2026-08-15）：这是
    // 最大收益项——接收端（C3，单核）原来每帧要经主循环（优先级 1）中转，
    // 而 esp_wifi 任务（优先级 23）收帧后持续占用 CPU，主循环迟迟得不到
    // 调度，每命令多 ~9ms；直通后 dap_task（优先级 24）立即抢占执行。
    // 发送端（S3，双核）直通收益小（~0.2s）。入队/拷贝均为非阻塞轻量
    // 操作，可安全在 esp_wifi 任务上下文执行。
    auto &app = Application::get_instance();
    switch (rp.packet.packet_type)
    {
    case PacketType::DapCommand: {
        const auto &cmd = rp.packet.payload.dap_command;
        const esp_err_t ret = app.wireless_link().dap_execute(rp.src_mac, cmd.data, cmd.len);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to enqueue DAP command from " MACSTR ": %s", MAC2STR(rp.src_mac),
                     esp_err_to_name(ret));
        }
        return;
    }
    case PacketType::DapResponse: {
        const auto &resp = rp.packet.payload.dap_response;
        app.push_wireless_response(resp.data, resp.len);
        return;
    }
    case PacketType::UartData: {
        // 串口数据直通投递（不经主循环）：与 DAP 命令同路径优化。接收端
        // 动作由本机角色决定——USB 侧（S3）写 CDC，无 USB 侧（C3）写 UART
        // （见 Application::on_uart_data）。入队/拷贝均为轻量操作，可安全
        // 在 esp_wifi 任务上下文执行。
        const auto &uart = rp.packet.payload.uart_data;
        app.on_uart_data(uart.data, uart.len);
        return;
    }
    case PacketType::UartLineCoding: {
        // 线编码同步直通投递：PC 端改串口参数 → 目标侧 UART 实时跟随
        const auto &lc = rp.packet.payload.uart_line_coding;
        app.on_uart_line_coding(lc);
        return;
    }
    case PacketType::RelayedUsbControl: {
        // 对端转发的 USB 控制请求（无 USB 侧的设置下发通路）。与 USB 侧同一张
        // 命令表：本模块只解包投递，执行在主循环——回调不可重入 WiFi 驱动，
        // 速率一类会操作驱动，必须留给主循环。
        const auto &relay = rp.packet.payload.relayed_usb_control;
        app.config_channel().on_relayed_control(relay);
        return;
    }
    default:
        // 配对控制帧（Probe/PairingRequest/PairingAccepted）：维持经主循环
        // 统一驱动配对状态机的原路径
        break;
    }

    Application::get_instance().push_received_packet(rp);
}

void WirelessLink::send_cb([[maybe_unused]] const wifi_tx_info_t *send_info, esp_now_send_status_t send_status)
{
    if (send_status != ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGW(TAG, "ESP-NOW send failed");
    }
}

esp_err_t WirelessLink::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    m_tx_mutex = xSemaphoreCreateMutex();
    if (m_tx_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 注入丢帧的随机序列：两端各自用 esp_random 播种，不共享序列——固定种子会
    // 让两台设备在相同帧号上同时丢帧，制造出现实里不存在的人为相关
    m_loss_rng = esp_random() | 1u;
#endif

    esp_err_t ret;
    ret = mlink::nvs_flash_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "mlink::nvs_flash_init() failed");

    ret = esp_netif_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_netif_init() failed");
    ret = esp_event_loop_create_default();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_event_loop_create_default() failed");

    {
        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&wifi_init_cfg);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_init() failed");
        ESP_LOGI(TAG, "WiFi initialized");

        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_storage() failed");
    }

    {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_mode() failed");

        wifi_config_t wifi_cfg = {};
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_config() failed");

        ret = esp_wifi_start();
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_start() failed");

        // 禁用 WiFi 省电（modem sleep）。A/B 实测（2026-08-15，两端一致）：
        // PS 默认（MIN_MODEM）3.28-3.40s vs PS 禁用 3.12-3.17s，禁用省电
        // 贡献 ~0.2s（往返 ~0.3ms/命令）。保留禁用。
        ret = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s", esp_err_to_name(ret));

        ret = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_wifi_set_channel() failed");
    }

    ret = esp_now_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_now_init() failed");

    ret = peer_add(ESP_NOW_BROADCAST);
    ESP_RETURN_ON_ERROR(ret, TAG, "peer_add(ESP_NOW_BROADCAST) failed: %s", esp_err_to_name(ret));

    ret = esp_now_register_recv_cb(recv_cb);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to register ESP-NOW receive callback: %s", esp_err_to_name(ret));

    ret = esp_now_register_send_cb(send_cb);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to register ESP-NOW send callback: %s", esp_err_to_name(ret));

    // 无线侧 DAP 执行器：命令队列 + 高优先级任务（收到 DapCommand → 执行 → 回发）
    m_dap_queue = xQueueCreate(DAP_QUEUE_DEPTH, sizeof(DapExecuteRequest));
    if (m_dap_queue == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create DAP command queue");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    // 优先级 24（> esp_wifi 任务 23）：单核（ESP32-C3）上保证 SWD 位碰撞
    // 执行期间不被任务抢占（抢占会破坏 SWCLK 时序，见 usb.cc 注释）；
    // 任务平时阻塞在队列接收上，不占用 CPU，短暂最高优先级无害。
    // 栈 4096 字节：DAP 执行调用链（DAP_ExecuteCommand 等）在栈上运行。
    if (xTaskCreate(dap_task_fn, "dap-wireless", 4096, this, 24, &m_dap_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create wireless DAP executor task");
        vQueueDelete(m_dap_queue);
        m_dap_queue = nullptr;
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    m_initialized = true;
    return ESP_OK;

err:
    return ret;
}

esp_err_t WirelessLink::peer_add(const u8 *peer_mac)
{
    if (esp_now_is_peer_exist(peer_mac))
    {
        // 已存在也确保速率配置正确（重复调用幂等）。
        return set_peer_rate(peer_mac);
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
        return ret;
    }

    return set_peer_rate(peer_mac);
}

esp_err_t WirelessLink::set_peer_rate(const u8 *peer_mac)
{
    if (peer_mac == nullptr || std::memcmp(peer_mac, ESP_NOW_BROADCAST, ESP_NOW_ETH_ALEN) == 0)
    {
        return ESP_OK;
    }

    // 对端级固定速率由 m_rate 决定。ESP-NOW 的速率以 peer rate config 为准，
    // 仅设全局接口速率不一定生效；档位越低越稳，长帧尤其明显。
    wifi_phy_mode_t phymode = WIFI_PHY_MODE_HT20;
    if (!phymode_for_rate(m_rate, &phymode))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_now_rate_config_t rate_cfg = {};
    rate_cfg.phymode = phymode;
    rate_cfg.rate = static_cast<wifi_phy_rate_t>(m_rate);
    rate_cfg.ersu = false;
    rate_cfg.dcm = false;
    const esp_err_t ret = esp_now_set_peer_rate_config(peer_mac, &rate_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_now_set_peer_rate_config(0x%02X) failed: %s", m_rate, esp_err_to_name(ret));
    }
    return ret;
}

u8 WirelessLink::wifi_rate() const
{
    return m_rate;
}

esp_err_t WirelessLink::set_wifi_rate(u8 rate)
{
    wifi_phy_mode_t phymode = WIFI_PHY_MODE_HT20;
    if (!phymode_for_rate(rate, &phymode))
    {
        ESP_LOGW(TAG, "Unsupported WiFi rate 0x%02X", rate);
        return ESP_ERR_INVALID_ARG;
    }

    m_rate = rate;

    // 广播 peer 不支持速率配置；已配对单播 peer 直接改配置，无需重建。
    // 对端侧的同一档位由配置通道经 RelayedUsbControl 下发（见 config_channel）。
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (peer_mac_load(peer_mac))
    {
        const esp_err_t ret = set_peer_rate(peer_mac);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    ESP_LOGI(TAG, "WiFi rate set to 0x%02X (phymode %d)", rate, static_cast<int>(phymode));
    return ESP_OK;
}

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
u8 WirelessLink::loss_permille() const
{
    return m_loss_permille;
}

esp_err_t WirelessLink::set_loss_permille(u8 permille)
{
    if (permille > MAX_LOSS_PERMILLE)
    {
        ESP_LOGW(TAG, "Loss permille out of range: %u (max %u)", permille, MAX_LOSS_PERMILLE);
        return ESP_ERR_INVALID_ARG;
    }

    m_loss_permille = permille;
    // 换档即重播种：同一档位反复实验不会重放同一串丢帧
    m_loss_rng = esp_random() | 1u; // xorshift 的 0 是不动点

    // 无 USB 侧（接收端）没有宿主可以读回配置，它有没有收到中继只能靠这条日志
    ESP_LOGI(TAG, "Link loss injection: %u permille (%u.%u%%)", permille, permille / 10, permille % 10);
    return ESP_OK;
}

u32 WirelessLink::next_random()
{
    u32 x = m_loss_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_loss_rng = x;
    return x;
}

bool WirelessLink::loss_should_drop(PacketType type)
{
    // 白名单：只有 DAP 与串口数据帧过这道闸。配对、探测与配置通道控制帧一律
    // 放行，丢包率因此永远丢不掉"改回来"的那条控制请求。
    if (type != PacketType::DapCommand && type != PacketType::DapResponse && type != PacketType::UartData)
    {
        return false;
    }

    auto &config = Application::get_instance().config_channel();
    const u8 permille = m_loss_permille;
    if (permille == 0)
    {
        config.add_loss_frame(false);
        return false;
    }

    const bool drop = (next_random() % 1000u) < permille;
    config.add_loss_frame(drop);
    return drop;
}
#endif

void WirelessLink::poll()
{
    portENTER_CRITICAL(&m_rate_lock);
    const u32 pending = m_pending_rate;
    m_pending_rate = 0;
    portEXIT_CRITICAL(&m_rate_lock);

    if (pending == 0)
    {
        return;
    }

    const u8 rate = static_cast<u8>(pending & 0xFF);
    if (rate == m_rate)
    {
        return;
    }

    // 只应用不回传：回传会让两端互相触发
    (void)set_wifi_rate(rate);
}

esp_err_t WirelessLink::peer_remove(const u8 *peer_mac)
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

esp_err_t WirelessLink::send(const u8 *dest_mac, const Packet &packet)
{
    // 发送帧缓冲为对象成员：DAP 帧可达 1470B，放栈上会吃掉发送方任务的栈。
    // DAP 帧丢失会让主机端命令与响应错位，必须拿到发送权；其余帧尽力而为，
    // 取不到就直接丢弃，避免在 ESP-NOW 回调里阻塞。
    const bool reliable = (packet.packet_type == PacketType::DapCommand) ||
                          (packet.packet_type == PacketType::DapResponse);
    if (m_tx_mutex == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(m_tx_mutex, reliable ? portMAX_DELAY : 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t ret = packet.serialize(m_tx_frame, sizeof(m_tx_frame));
    if (ret != ESP_OK)
    {
        xSemaphoreGive(m_tx_mutex);
        ESP_LOGE(TAG, "Failed to serialize packet: %s", esp_err_to_name(ret));
        return ret;
    }

    const size_t frame_len = packet.serialized_size();

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 注入丢帧。被丢的帧照旧计入 wl_tx_bytes：真实丢包时 esp_now_send 也是返回
    // 成功、这个计数照样加，两种丢帧因此在统计上不可区分（差别只在 send_cb
    // 那条 ESP-NOW send failed 日志——注入不产生它，逐帧日志会扰动时序）。
    if (loss_should_drop(packet.packet_type))
    {
        xSemaphoreGive(m_tx_mutex);
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(frame_len));
        return ESP_OK;
    }
#endif

    const esp_err_t send_ret = esp_now_send(dest_mac, m_tx_frame, frame_len);
    xSemaphoreGive(m_tx_mutex);

    if (send_ret == ESP_OK)
    {
        // 链路层统计：成功交给 ESP-NOW 的帧（发送结果由 send_cb 另行处理，
        // 这里只反映"已发出"的字节量）
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(frame_len));
    }
    return send_ret;
}

esp_err_t WirelessLink::send_dap_command(const u8 *dest_mac, const u8 *data, size_t len)
{
    if (len > MAX_DAP_PAYLOAD_SIZE)
    {
        ESP_LOGE(TAG, "DAP command too large (%u bytes, max %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(MAX_DAP_PAYLOAD_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：data 指向调用方命令缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::DapCommand;
    auto &content = packet.payload.dap_command;
    content.len = static_cast<u16>(len);
    content.data = data;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_dap_response(const u8 *dest_mac, const u8 *data, size_t len)
{
    if (len > MAX_DAP_PAYLOAD_SIZE)
    {
        ESP_LOGE(TAG, "DAP response too large (%u bytes, max %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(MAX_DAP_PAYLOAD_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：data 指向调用方响应缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::DapResponse;
    auto &content = packet.payload.dap_response;
    content.len = static_cast<u16>(len);
    content.data = data;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_uart_data(const u8 *dest_mac, const u8 *data, size_t len)
{
    if (len > MAX_UART_PAYLOAD_SIZE)
    {
        ESP_LOGE(TAG, "UART data too large (%u bytes, max %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(MAX_UART_PAYLOAD_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：data 指向调用方串口缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::UartData;
    auto &content = packet.payload.uart_data;
    content.len = static_cast<u16>(len);
    content.data = data;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_uart_line_coding(const u8 *dest_mac, u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    Packet packet;
    packet.packet_type = PacketType::UartLineCoding;
    auto &content = packet.payload.uart_line_coding;
    content.baud = baud;
    content.data_bits = data_bits;
    content.stop_bits = stop_bits;
    content.parity = parity;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_ping(const u8 *dest_mac, u32 token)
{
    Packet packet;
    packet.packet_type = PacketType::Ping;
    packet.payload.ping.token = token;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_pong(const u8 *dest_mac, u32 token)
{
    Packet packet;
    packet.packet_type = PacketType::Pong;
    packet.payload.pong.token = token;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::send_relayed_usb_control(const u8 *dest_mac, const payload::RelayedUsbControl &req)
{
    if (req.len > payload::MAX_RELAYED_CONTROL_PAYLOAD || (req.len > 0 && req.data == nullptr))
    {
        ESP_LOGE(TAG, "Relayed control payload invalid (len %u, max %u)", static_cast<unsigned>(req.len),
                 static_cast<unsigned>(payload::MAX_RELAYED_CONTROL_PAYLOAD));
        return ESP_ERR_INVALID_SIZE;
    }

    // Packet 为指针视图：载荷指向调用方缓冲，零拷贝
    Packet packet;
    packet.packet_type = PacketType::RelayedUsbControl;
    packet.payload.relayed_usb_control = req;
    return send(dest_mac, packet);
}

esp_err_t WirelessLink::forward_dap_command(const u8 *cmd, size_t len, u8 *resp, size_t resp_cap, u16 *resp_len)
{
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (!mlink::peer_mac_load(peer_mac))
    {
        ESP_LOGW(TAG, "Wireless: no paired peer, %u-byte DAP request dropped", static_cast<unsigned>(len));
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = send_dap_command(peer_mac, cmd, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wireless: failed to forward %u-byte DAP request: %s", static_cast<unsigned>(len),
                 esp_err_to_name(ret));
        return ret;
    }
    // 性能诊断（DEBUG 级）：转发命令发出的时刻。配合接收端（C3）的
    // Executing/Done 日志与下方的"收到响应"日志，可分离 USB/ESP-NOW/执行
    // 各段延迟（无线 DAP 路径单命令往返约 12ms 的实测瓶颈定位用）。
    const TickType_t sent_tick = xTaskGetTickCount();
    ESP_LOGD(TAG, "Wireless: DAP request %uB sent (tick=%u)", static_cast<unsigned>(len),
             static_cast<unsigned>(sent_tick));

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
    ESP_LOGD(TAG, "Wireless: DAP response %uB received (tick=%u, wireless_rtt=%u ms)", static_cast<unsigned>(rlen),
             static_cast<unsigned>(xTaskGetTickCount()), static_cast<unsigned>(xTaskGetTickCount() - sent_tick));

    if (rlen > resp_cap)
    {
        // 防御性截断（正常不会触发：接收端只发送 ≤ MAX_DAP_PAYLOAD_SIZE 的响应）
        ESP_LOGW(TAG, "Wireless response truncated (%u -> %u)", rlen, static_cast<unsigned>(resp_cap));
        rlen = static_cast<u16>(resp_cap);
    }
    *resp_len = rlen;
    return ESP_OK;
}

// ---- 无线侧 DAP 执行接线 ----
// 与 usb.cc 的 dap_task 对称：无线传输层负责把收到的 DapCommand 交给公共
// 执行器（dap_executor_run）执行，并把结果以 DapResponse 帧回发给来源。
// 执行循环、DAP_Setup 与命令日志均在 dap_executor.cc，这里只有队列、
// 任务与输入/输出回调；状态全在 WirelessLink 对象内。

bool WirelessLink::dap_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);

    DapExecuteRequest req;
    if (xQueueReceive(link->m_dap_queue, &req, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    if (req.len > buf_cap)
    {
        // 防御：命令不得超过执行器请求缓冲（DAP_MAX_REQUEST_SIZE）
        ESP_LOGW(TAG, "DAP command too large (%u bytes, cap %u), dropped", static_cast<unsigned>(req.len),
                 static_cast<unsigned>(buf_cap));
        return false;
    }
    std::memcpy(buf, req.data, req.len);
    *len = req.len;
    std::memcpy(link->m_dap_src_mac, req.src_mac, ESP_NOW_ETH_ALEN);

    // 无线接收端只有这一条 DAP 通道：DAP_Info 按无线单帧上限上报，
    // 主机据此决定一条命令能携带多少数据
    DAP_Data.packet_size = static_cast<u16>(MAX_DAP_PAYLOAD_SIZE);
    return true;
}

void WirelessLink::dap_resp_sink(const u8 *data, u16 len, void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);
    const esp_err_t ret = link->send_dap_response(link->m_dap_src_mac, data, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send DAP response: %s", esp_err_to_name(ret));
    }
}

void WirelessLink::dap_task_fn(void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);
    // 复用公共执行器：取命令（无线队列）→ 执行 → 响应回发；
    // 命令/响应缓冲用对象成员（不占任务栈，见 dap_executor.hpp）
    dap_executor_run(WirelessLink::dap_cmd_src, WirelessLink::dap_resp_sink, arg, link->m_dap_request,
                     sizeof(link->m_dap_request), link->m_dap_response, sizeof(link->m_dap_response));
}

esp_err_t WirelessLink::dap_execute(const u8 *src_mac, const u8 *data, size_t len)
{
    if (m_dap_queue == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > MAX_DAP_PAYLOAD_SIZE)
    {
        ESP_LOGW(TAG, "DAP command too large (%u bytes, max %u), dropped", static_cast<unsigned>(len),
                 static_cast<unsigned>(MAX_DAP_PAYLOAD_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    DapExecuteRequest req = {};
    std::memcpy(req.src_mac, src_mac, ESP_NOW_ETH_ALEN);
    req.len = static_cast<u16>(len);
    std::memcpy(req.data, data, len);

    if (xQueueSend(m_dap_queue, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "DAP command queue full, command dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---- Packet 序列化 ----

#define MLINK_CHECK_SIZE(parser, expected_size)                                                                        \
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
        return DAP_FRAME_HEADER_SIZE + len;
    }
    case PacketType::UartData: {
        return UART_FRAME_HEADER_SIZE + payload.uart_data.len;
    }
    case PacketType::UartLineCoding: {
        // 帧头 3B + 数据段 7B（baud 4B + 3 个 1B）
        return UART_FRAME_HEADER_SIZE + 7;
    }
    case PacketType::Ping:
    case PacketType::Pong: {
        // 帧头 3B + 数据段 4B（token）
        return UART_FRAME_HEADER_SIZE + 4;
    }
    case PacketType::RelayedUsbControl: {
        // 帧头 3B + len 2B + SETUP 包 8B + 载荷
        return UART_FRAME_HEADER_SIZE + 2 + payload::RELAYED_CONTROL_HEADER_SIZE +
               payload.relayed_usb_control.len;
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

    MLINK_CHECK_SIZE(p, 1);
    packet_type = static_cast<PacketType>(p.read_u8());

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        MLINK_CHECK_SIZE(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        MLINK_CHECK_SIZE(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        // 数据段拷入调用方存储，data 指向该存储（见 ReceivedPacket::storage）
        auto &content = payload.dap_command;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::UartData: {
        auto &content = payload.uart_data;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::UartLineCoding: {
        // 数据段固定 7B：baud 4B + data_bits/stop_bits/parity 各 1B
        auto &content = payload.uart_line_coding;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 7B）
        MLINK_CHECK_SIZE(p, 7);
        content.baud = p.read_u32();
        content.data_bits = p.read_u8();
        content.stop_bits = p.read_u8();
        content.parity = p.read_u8();
        break;
    }
    case PacketType::Ping: {
        // 数据段固定 4B：token
        auto &content = payload.ping;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 4B）
        MLINK_CHECK_SIZE(p, 4);
        content.token = p.read_u32();
        break;
    }
    case PacketType::Pong: {
        auto &content = payload.pong;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 4B）
        MLINK_CHECK_SIZE(p, 4);
        content.token = p.read_u32();
        break;
    }
    case PacketType::RelayedUsbControl: {
        // 数据段：[len:2][SETUP 8B][载荷:len]；载荷拷入调用方存储，data 指向该存储
        auto &content = payload.relayed_usb_control;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap || n > payload::MAX_RELAYED_CONTROL_PAYLOAD)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, payload::RELAYED_CONTROL_HEADER_SIZE);
        content.bm_request_type = p.read_u8();
        content.b_request = p.read_u8();
        content.w_value = p.read_u16();
        content.w_index = p.read_u16();
        content.w_length = p.read_u16();
        MLINK_CHECK_SIZE(p, n);
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
    MLINK_CHECK_SIZE(s, 1);
    s.write_u8(static_cast<u8>(packet_type));

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        MLINK_CHECK_SIZE(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        MLINK_CHECK_SIZE(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        auto &content = payload.dap_command;
        if (content.data == nullptr || content.len > MAX_DAP_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        if (content.data == nullptr || content.len > MAX_DAP_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::UartData: {
        auto &content = payload.uart_data;
        if (content.data == nullptr || content.len > MAX_UART_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::UartLineCoding: {
        auto &content = payload.uart_line_coding;
        MLINK_CHECK_SIZE(s, 2 + 7);
        s.write_u16(7);
        s.write_u32(content.baud);
        s.write_u8(content.data_bits);
        s.write_u8(content.stop_bits);
        s.write_u8(content.parity);
        break;
    }
    case PacketType::Ping: {
        auto &content = payload.ping;
        MLINK_CHECK_SIZE(s, 2 + 4);
        s.write_u16(4);
        s.write_u32(content.token);
        break;
    }
    case PacketType::Pong: {
        auto &content = payload.pong;
        MLINK_CHECK_SIZE(s, 2 + 4);
        s.write_u16(4);
        s.write_u32(content.token);
        break;
    }
    case PacketType::RelayedUsbControl: {
        auto &content = payload.relayed_usb_control;
        if (content.len > payload::MAX_RELAYED_CONTROL_PAYLOAD ||
            (content.len > 0 && content.data == nullptr))
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2 + payload::RELAYED_CONTROL_HEADER_SIZE);
        s.write_u16(content.len);
        s.write_u8(content.bm_request_type);
        s.write_u8(content.b_request);
        s.write_u16(content.w_value);
        s.write_u16(content.w_index);
        s.write_u16(content.w_length);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef MLINK_CHECK_SIZE
