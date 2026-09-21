#include "meguminlink/wireless/link.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp" // dap_executor_run（无线侧执行接线）
#include "meguminlink/dap_frame.hpp"
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

/** 一条待执行的 DAP 命令：命令来源（响应回发）+ 可靠传输序号 + 原样命令字节流 */
struct DapExecuteRequest
{
    u8 src_mac[ESP_NOW_ETH_ALEN];
    u16 cmd_seq;
    u16 len;
    u8 data[MAX_DAP_PAYLOAD_SIZE];
};

/** 单调毫秒时钟：可靠传输的时间口径（tick 换算，1000Hz 下 tick 即毫秒） */
u32 now_ms()
{
    return static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

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

    // 反序列化存储随无线对象持有：数据段可达 MAX_DAP_PAYLOAD_SIZE，放栈上会
    // 吃掉 ESP-NOW 回调所在任务的栈。
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

    auto &app = Application::get_instance();

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 任何帧都捎带确认，先统一喂给发送窗口；接收回调里只更新状态与待发标记，
    // 出向帧（重传、纯确认、重放、握手）一律留给 poll——回调里发帧会重入驱动
    const u32 arq_now = now_ms();
    link.m_arq.on_ack(rp.packet.arq.ack, rp.packet.arq.sack, rp.packet.arq.window, arq_now);
#endif

    // DAP 数据帧直通投递（不经主循环）。A/B 实测（2026-08-15）：这是
    // 最大收益项——接收端（C3，单核）原来每帧要经主循环（优先级 1）中转，
    // 而 esp_wifi 任务（优先级 23）收帧后持续占用 CPU，主循环迟迟得不到
    // 调度，每命令多 ~9ms；直通后 dap_task（优先级 24）立即抢占执行。
    // 发送端（S3，双核）直通收益小（~0.2s）。入队/拷贝均为非阻塞轻量
    // 操作，可安全在 esp_wifi 任务上下文执行。
    switch (rp.packet.packet_type)
    {
    case PacketType::DapCommand: {
        const auto &cmd = rp.packet.payload.dap_command;
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
        // 可靠消息先过适配层：新帧才交付执行，重复帧只记下待重放，绝不重新执行
        link.m_arq.on_message(rp.packet.arq.seq, (rp.packet.arq.flags & ARQ_FLAG_GAP_BEFORE) != 0, cmd.data, cmd.len,
                              arq_now);
#else
        const esp_err_t ret = app.wireless_link().dap_execute(rp.src_mac, 0, cmd.data, cmd.len);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to enqueue DAP command from " MACSTR ": %s", MAC2STR(rp.src_mac),
                     esp_err_to_name(ret));
        }
#endif
        return;
    }
    case PacketType::DapResponse: {
        const auto &resp = rp.packet.payload.dap_response;
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
        link.m_arq.on_message(rp.packet.arq.seq, (rp.packet.arq.flags & ARQ_FLAG_GAP_BEFORE) != 0, resp.data,
                              resp.len, arq_now);
#else
        app.push_wireless_response(resp.data, resp.len);
#endif
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
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    case PacketType::Ack: {
        // 纯确认帧没有帧体：确认信息在上面的通用路径里已经消费掉了
        return;
    }
    case PacketType::ArqReset: {
        // 对端开了新会话：把本端序号基准拉回去对齐，并按代数决定是否采纳它的参数
        // （迟到帧携带旧 epoch，会被挡住）；flags.echo 区分请求与回显，回显不再回显
        link.m_arq.on_reset(rp.packet.payload.arq_reset.epoch, rp.packet.payload.arq_reset.settings,
                            rp.packet.payload.arq_reset.flags.echo, arq_now);
        return;
    }
#endif
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

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    {
        // 可靠传输适配层：静态缓冲按窗口上限一次分配，必须在注册 ESP-NOW 回调
        // 之前就绪（回调一注册就可能进帧）。重排缓冲本步不启用——窗口还是 1，
        // 乱序帧一律拒绝并由对端重传，无需额外的载荷存储。
        static_assert(MAX_DAP_PAYLOAD_SIZE <= ArqLink::MAX_PAYLOAD, "wire payload exceeds ARQ buffer");

        // 参数从 NVS 读回（无记录即默认值）；窗口上限另由 Kconfig 约束，见 arq_link
        const esp_err_t arq_ret = m_arq.init(arq_settings_load(), MAX_DAP_PAYLOAD_SIZE);
        ESP_RETURN_ON_ERROR(arq_ret, TAG, "m_arq.init() failed");

        const ArqLink::Callbacks arq_callbacks = {this, &WirelessLink::arq_transmit, &WirelessLink::arq_send_ack,
                                                  &WirelessLink::arq_send_reset, &WirelessLink::arq_deliver};
        m_arq.set_callbacks(arq_callbacks);
    }
#endif

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
    Packet outgoing = packet;

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 可靠消息的序号、标志与确认由适配层填好（它自己调 send_raw），这里只给
    // 其余帧捎带本端接收侧的确认——任何一帧都能当确认用，这才让"没有数据可发
    // 时的确认"不至于白占一帧。
    if (packet.packet_type != PacketType::DapCommand && packet.packet_type != PacketType::DapResponse)
    {
        const ArqLink::Piggyback piggyback = m_arq.take_piggyback();
        outgoing.arq.ack = piggyback.ack;
        outgoing.arq.sack = piggyback.sack;
        outgoing.arq.window = piggyback.window;
    }
#endif

    return send_raw(dest_mac, outgoing);
}

esp_err_t WirelessLink::send_raw(const u8 *dest_mac, const Packet &packet)
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

    auto &app = Application::get_instance();

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 会话握手（ArqReset）必须先完成：两端序号基准没对齐时发出去的命令会被对端
    // 当成重复帧丢掉，而对端回报的累积确认号又会把它"确认"掉，命令静默消失。
    // 握手是周期重发 + 回显，两端同时上电时通常几十毫秒就完成。
    m_arq.set_peer(peer_mac, now_ms());
    const TickType_t handshake_start = xTaskGetTickCount();
    while (!m_arq.established())
    {
        arq_poll(now_ms());
        if (static_cast<u32>((xTaskGetTickCount() - handshake_start) * portTICK_PERIOD_MS) >=
            ArqLink::HANDSHAKE_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "Wireless: ARQ session not aligned within %u ms, %u-byte request dropped",
                     static_cast<unsigned>(ArqLink::HANDSHAKE_TIMEOUT_MS), static_cast<unsigned>(len));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // 上一条命令超时后迟到的响应必须清掉：留着会被下一条命令当成自己的响应收走，
    // 主机会看到响应与命令错位
    u16 stale = 0;
    while (app.wait_wireless_response(resp, &stale, 0))
    {
    }

    u16 seq = 0;
    const ArqLink::Submit submitted = m_arq.submit(cmd, static_cast<u16>(len), now_ms(), &seq);
    if (submitted != ArqLink::Submit::Ok)
    {
        ESP_LOGW(TAG, "Wireless: cannot submit %u-byte DAP request (result %d)", static_cast<unsigned>(len),
                 static_cast<int>(submitted));
        return ESP_ERR_TIMEOUT;
    }
    // 性能诊断（DEBUG 级）：转发命令发出的时刻。配合接收端（C3）的
    // Executing/Done 日志与下方的"收到响应"日志，可分离 USB/ESP-NOW/执行
    // 各段延迟（无线 DAP 路径单命令往返约 12ms 的实测瓶颈定位用）。
    const TickType_t sent_tick = xTaskGetTickCount();
    ESP_LOGD(TAG, "Wireless: DAP request %uB seq=%u sent (tick=%u)", static_cast<unsigned>(len), seq,
             static_cast<unsigned>(sent_tick));

    // 等到响应为止：等待时长取"硬超时"与"可靠传输下一个待办时刻"的较小者，
    // 到点醒来驱动重传与确认，因此不必轮询也能及时补帧
    const TickType_t hard_deadline = sent_tick + pdMS_TO_TICKS(CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS);
    u16 rlen = 0;
    for (;;)
    {
        const u32 now = now_ms();
        arq_poll(now);

        const TickType_t remaining = hard_deadline - xTaskGetTickCount();
        if (static_cast<i32>(remaining) <= 0)
        {
            break;
        }

        u32 wait_ms = static_cast<u32>(remaining) * portTICK_PERIOD_MS;
        const u32 arq_deadline = m_arq.next_deadline_ms();
        if (arq_deadline != 0)
        {
            const u32 until_arq = static_cast<u32>(static_cast<i32>(arq_deadline - now));
            if (until_arq < wait_ms)
            {
                wait_ms = until_arq;
            }
        }
        if (wait_ms == 0)
        {
            wait_ms = 1;
        }

        if (app.wait_wireless_response(resp, &rlen, wait_ms))
        {
            ESP_LOGD(TAG, "Wireless: DAP response %uB received (tick=%u, wireless_rtt=%u ms)",
                     static_cast<unsigned>(rlen), static_cast<unsigned>(xTaskGetTickCount()),
                     static_cast<unsigned>(xTaskGetTickCount() - sent_tick));
            if (rlen > resp_cap)
            {
                // 防御性截断（正常不会触发：接收端只发送 ≤ MAX_DAP_PAYLOAD_SIZE 的响应）
                ESP_LOGW(TAG, "Wireless response truncated (%u -> %u)", rlen, static_cast<unsigned>(resp_cap));
                rlen = static_cast<u16>(resp_cap);
            }
            *resp_len = rlen;
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Wireless: no response within %u ms, %u-byte request dropped",
             CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS, static_cast<unsigned>(len));
    return ESP_ERR_TIMEOUT;
#else
    const esp_err_t ret = send_dap_command(peer_mac, cmd, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wireless: failed to forward %u-byte DAP request: %s", static_cast<unsigned>(len),
                 esp_err_to_name(ret));
        return ret;
    }
    const TickType_t sent_tick = xTaskGetTickCount();
    ESP_LOGD(TAG, "Wireless: DAP request %uB sent (tick=%u)", static_cast<unsigned>(len),
             static_cast<unsigned>(sent_tick));

    // 阻塞等待接收端响应（一次一个在途请求，与 CMSIS-DAP v2 同步请求/响应
    // 语义一致；等待期间不处理下一个请求）。
    u16 rlen = 0;
    if (!app.wait_wireless_response(resp, &rlen, CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "Wireless: no response within %u ms, %u-byte request dropped",
                 CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS, static_cast<unsigned>(len));
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGD(TAG, "Wireless: DAP response %uB received (tick=%u, wireless_rtt=%u ms)", static_cast<unsigned>(rlen),
             static_cast<unsigned>(xTaskGetTickCount()), static_cast<unsigned>(xTaskGetTickCount() - sent_tick));

    if (rlen > resp_cap)
    {
        ESP_LOGW(TAG, "Wireless response truncated (%u -> %u)", rlen, static_cast<unsigned>(resp_cap));
        rlen = static_cast<u16>(resp_cap);
    }
    *resp_len = rlen;
    return ESP_OK;
#endif
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
    link->m_dap_cmd_seq = req.cmd_seq;

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 执行前先驱动一次可靠传输：把"命令已送达"这类待发帧（纯确认、重放）送出去，
    // 免得它们被本条命令那段几十毫秒的执行时间压在后面
    link->arq_poll(now_ms());
#endif

    // 无线接收端只有这一条 DAP 通道：DAP_Info 上报值受 CMSIS-DAP 单字节
    // Transfer Count 的安全上限约束（见 DAP_REPORTED_PACKET_SIZE_MAX），
    // 主机据此决定一条命令能携带多少数据
    DAP_Data.packet_size = static_cast<u16>(DAP_REPORTED_PACKET_SIZE_MAX);
    return true;
}

void WirelessLink::dap_resp_sink(const u8 *data, u16 len, void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 响应走可靠传输：序号与确认由适配层给，并记下它回的是哪条命令——对端重传
    // 那条命令时据此重放这条响应，而不是把命令再执行一遍
    const ArqLink::Submit result = link->m_arq.submit_response(link->m_dap_cmd_seq, data, len, now_ms());
    if (result != ArqLink::Submit::Ok)
    {
        ESP_LOGE(TAG, "Failed to submit DAP response: result %d", static_cast<int>(result));
    }
#else
    const esp_err_t ret = link->send_dap_response(link->m_dap_src_mac, data, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send DAP response: %s", esp_err_to_name(ret));
    }
#endif
}

void WirelessLink::dap_task_fn(void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);
    // 复用公共执行器：取命令（无线队列）→ 执行 → 响应回发；
    // 命令/响应缓冲用对象成员（不占任务栈，见 dap_executor.hpp）
    dap_executor_run(WirelessLink::dap_cmd_src, WirelessLink::dap_resp_sink, arg, link->m_dap_request,
                     sizeof(link->m_dap_request), link->m_dap_response, sizeof(link->m_dap_response));
}

esp_err_t WirelessLink::dap_execute(const u8 *src_mac, u16 cmd_seq, const u8 *data, size_t len)
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
    req.cmd_seq = cmd_seq;
    req.len = static_cast<u16>(len);
    std::memcpy(req.data, data, len);

    if (xQueueSend(m_dap_queue, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "DAP command queue full, command dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE

// ---- 可靠传输接线 ----
// 收：ESP-NOW 回调把帧喂进适配层（见 recv_cb），那里只更新状态与待发标记；
// 发：全部集中在 arq_poll 里——重传、纯确认、响应重放与会话握手帧。两边分开是
// 因为接收回调跑在 esp_wifi 任务上，在那里发帧会重入 WiFi 驱动。

void WirelessLink::arq_poll(u32 now)
{
    // 对端可能刚配对或刚换：让适配层拿到当前 peer（同一个对端重复调用是幂等的，
    // 只有真的换了对端才会重置会话）
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (mlink::peer_mac_load(peer_mac))
    {
        // 单播 peer 必须先在 ESP-NOW list 里，否则手上的帧一律发送失败。主循环
        // 稍后也会做同样的事（幂等），但握手帧排在它前面，这里补一次免得白等一轮
        (void)peer_add(peer_mac);
        m_arq.set_peer(peer_mac, now);
    }
    m_arq.poll(now);
}

bool WirelessLink::arq_established() const
{
    return m_arq.established();
}

u32 WirelessLink::arq_next_deadline_ms() const
{
    return m_arq.next_deadline_ms();
}

ArqLink::Snapshot WirelessLink::arq_snapshot() const
{
    return m_arq.snapshot();
}

ArqSettings WirelessLink::arq_settings() const
{
    return m_arq.settings();
}

esp_err_t WirelessLink::arq_apply_settings(const ArqSettings &settings, u32 now)
{
    return m_arq.apply_settings(settings, now);
}

bool WirelessLink::arq_take_settings_dirty()
{
    return m_arq.take_settings_dirty();
}

void WirelessLink::arq_transmit(void *ctx, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before,
                                u16 ack, u16 sack, u8 window)
{
    auto *self = static_cast<WirelessLink *>(ctx);

    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (!self->m_arq.peer_mac(peer_mac))
    {
        return;
    }

    // 本函数在适配层持锁时被调用，因此只能走 send_raw（它只取发送锁）
    Packet packet;
    packet.packet_type = ARQ_MESSAGE_TYPE;
    packet.arq.seq = seq;
    packet.arq.ack = ack;
    packet.arq.sack = sack;
    packet.arq.window = window;
    packet.arq.flags = ARQ_FLAG_RELIABLE;
    if (retransmit)
    {
        packet.arq.flags |= ARQ_FLAG_RETRANSMIT;
    }
    if (gap_before)
    {
        packet.arq.flags |= ARQ_FLAG_GAP_BEFORE;
    }

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    // USB 侧发送的是命令
    packet.payload.dap_command.len = len;
    packet.payload.dap_command.data = payload;
#else
    // 无 USB 侧发送的是响应
    packet.payload.dap_response.len = len;
    packet.payload.dap_response.data = payload;
#endif

    const esp_err_t ret = self->send_raw(peer_mac, packet);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send ARQ message seq=%u: %s", seq, esp_err_to_name(ret));
    }
}

void WirelessLink::arq_send_ack(void *ctx, u16 ack, u16 sack, u8 window)
{
    auto *self = static_cast<WirelessLink *>(ctx);

    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (!self->m_arq.peer_mac(peer_mac))
    {
        return;
    }

    Packet packet;
    packet.packet_type = PacketType::Ack;
    packet.arq.seq = 0;
    packet.arq.ack = ack;
    packet.arq.sack = sack;
    packet.arq.window = window;
    packet.arq.flags = 0;
    const esp_err_t ack_ret = self->send_raw(peer_mac, packet);
    if (ack_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send ARQ ack: %s", esp_err_to_name(ack_ret));
    }
}

void WirelessLink::arq_send_reset(void *ctx, u32 epoch, const ArqSettings &settings, bool echo, u16 ack, u16 sack,
                                  u8 window)
{
    auto *self = static_cast<WirelessLink *>(ctx);

    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (!self->m_arq.peer_mac(peer_mac))
    {
        return;
    }

    Packet packet;
    packet.packet_type = PacketType::ArqReset;
    packet.arq.seq = 0;
    packet.arq.ack = ack;
    packet.arq.sack = sack;
    packet.arq.window = window;
    packet.arq.flags = 0;
    packet.payload.arq_reset.epoch = epoch;
    packet.payload.arq_reset.settings = settings;
    packet.payload.arq_reset.flags.echo = echo;
    const esp_err_t reset_ret = self->send_raw(peer_mac, packet);
    if (reset_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send ARQ reset: %s", esp_err_to_name(reset_ret));
    }
}

void WirelessLink::arq_deliver(void *ctx, const u8 *peer_mac, u16 seq, const u8 *payload, u16 len)
{
    auto *self = static_cast<WirelessLink *>(ctx);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    // USB 侧收到的是响应：交给正在等待的转发路径。写缓冲 + give 信号量都是
    // 非阻塞操作，因此在适配层持锁时调用是安全的。
    (void)self;
    (void)peer_mac;
    (void)seq;
    Application::get_instance().push_wireless_response(payload, len);
#else
    // 无 USB 侧收到的是命令：入执行队列，序号一并带上（回响应时按它记账）
    if (peer_mac == nullptr)
    {
        ESP_LOGW(TAG, "ARQ command seq=%u dropped: peer unknown", seq);
        return;
    }
    const esp_err_t ret = self->dap_execute(peer_mac, seq, payload, len);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to enqueue ARQ DAP command seq=%u: %s", seq, esp_err_to_name(ret));
    }
#endif
}

#endif // CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
