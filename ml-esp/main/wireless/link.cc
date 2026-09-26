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
/**
 * 无线侧 DAP 执行器：命令队列深度。
 *
 * 可靠传输的 ACK 只代表"帧到了"，不代表"执行完了"：发送端窗口一释放就继续发，
 * 而本端执行是串行的，队列会以"发送速率 - 执行速率"的差值持续积压，满了就只能
 * 丢命令（那条命令的响应永远不会产生，主机侧表现为超时）。深度按一次连续写入的
 * 最大积压预留——96KB 写入实测峰值约 29 条。
 */
constexpr size_t DAP_QUEUE_DEPTH = 32;

/** 熔断期探针 Ping 的 token：只用于自证链路可用，与配置通道的探测 token 互不干扰 */
constexpr u32 ARQ_PROBE_TOKEN = 0x4D4C5052; // "MLPR"

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
    if (recv_info == nullptr || recv_info->src_addr == nullptr || data == nullptr || data_len <= 0)
    {
        return;
    }

    // 链路层统计：收到的每一帧都计（含被判为畸形的帧——它们同样占用了空口与
    // ESP-NOW 任务），RSSI 随帧更新
    if (recv_info->rx_ctrl != nullptr)
    {
        Application::get_instance().config_channel().add_wireless_rx(static_cast<u16>(data_len),
                                                                     recv_info->rx_ctrl->rssi);
    }

    // 解析、判重、排序与业务分派都不在回调里做：回调只把原始帧交给 ARQ 层，
    // 那里有独立的 ARQ 任务。回调里做这些会拖住 esp_wifi 任务，而在回调里发帧
    // 更会重入 WiFi 驱动。
    Application::get_instance().wireless_link().m_arq.on_frame(recv_info->src_addr, data,
                                                               static_cast<size_t>(data_len));
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
    m_pack_mutex = xSemaphoreCreateMutex();
    if (m_tx_mutex == nullptr || m_pack_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    {
        // ARQ 层：自带任务与收发环，必须在注册 ESP-NOW 回调之前就绪（回调一注册
        // 就可能进帧）。载荷上限取业务包上限（最大的业务包是 DAP 命令），
        // frame.hpp 的 static_assert 保证它装得下。
        const size_t arq_payload_cap = DAP_FRAME_HEADER_SIZE + DAP_CHANNEL_FIELD_SIZE + MAX_DAP_PAYLOAD_SIZE;
        const esp_err_t arq_ret = m_arq.init(arq_settings_load(), arq_payload_cap);
        ESP_RETURN_ON_ERROR(arq_ret, TAG, "m_arq.init() failed");

        const ArqLink::Callbacks arq_callbacks = {this, &WirelessLink::arq_send_raw, &WirelessLink::arq_deliver};
        m_arq.set_callbacks(arq_callbacks);
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
    // 栈 8192：DAP 执行调用链（DAP_ExecuteCommand 等）全在栈上跑，命令与响应
    // 缓冲已经挪到对象成员，这里留的是执行链本身与余量（水位见 GetStats 0x83）。
    if (xTaskCreate(dap_task_fn, "dap-wireless", 8192, this, 24, &m_dap_task) != pdPASS)
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
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac != nullptr)
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

bool WirelessLink::loss_should_drop()
{
    // 可丢性由提交方声明（见 submit_packet 的 loss_injectable）：本函数只做
    // 概率判定。配对、探测与配置通道控制帧不参与，丢包率因此永远丢不掉
    // "改回来"的那条控制请求。
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
    // 0) 配对信息交给 ESP-NOW 与 ARQ 层：单播 peer 必须先登记进 peer list，否则
    //    手上的帧一律发送失败；ARQ 层也要知道会话对端才能握手。两者都幂等（只有
    //    真的换了对端才会重置会话），而且执行侧（Local 模式）同样需要——它不发起
    //    命令，但要能回应握手与响应。
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac != nullptr)
    {
        (void)peer_add(peer_mac);
        m_arq.set_peer(peer_mac);
    }

    // 1) 对端下发的速率档位
    portENTER_CRITICAL(&m_rate_lock);
    const u32 pending = m_pending_rate;
    m_pending_rate = 0;
    portEXIT_CRITICAL(&m_rate_lock);

    const u8 rate = static_cast<u8>(pending & 0xFF);
    if (pending != 0 && rate != m_rate)
    {
        // 只应用不回传：回传会让两端互相触发
        (void)set_wifi_rate(rate);
    }

    // 2) 交付路径登记下来的线编码参数。configure 会调 UART 驱动，而交付跑在 ARQ
    //    任务里（正在守着重传定时器），所以只登记、在这里应用。
    portENTER_CRITICAL(&m_line_coding_lock);
    const bool coding_pending = m_line_coding_pending;
    const PendingLineCoding coding = m_pending_line_coding;
    m_line_coding_pending = false;
    portEXIT_CRITICAL(&m_line_coding_lock);
    if (coding_pending)
    {
        Application::get_instance().uart_bridge().configure(coding.baud, coding.data_bits, coding.stop_bits,
                                                            coding.parity);
    }

    // 3) 熔断期的主动探针：主机可能根本没有命令可提交（空闲），光靠"提交时试探"
    //    恢复不了。打一个 Ping——不可靠、不占窗口、不动会话，对端回 Pong 就证明
    //    链路双向可用；交付路径的 Pong 分支会清掉熔断。
    const u32 now = now_ms();
    if (m_arq.snapshot().breaker_open && static_cast<i32>(now - m_arq_probe_tx_ms) >= 0)
    {
        const ArqSettings settings = m_arq.settings();
        m_arq_probe_tx_ms = now + settings.breaker_probe_ms;

        const u8 *probe_mac = peer_mac_key().cached();
        if (probe_mac != nullptr)
        {
            const esp_err_t ret = send_ping(probe_mac, ARQ_PROBE_TOKEN);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "ARQ probe ping failed: %s", esp_err_to_name(ret));
            }
            else
            {
                ESP_LOGW(TAG, "ARQ breaker open: probe ping sent");
            }
        }
    }
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
    // 走不可靠路径：这条路是探测、配对与配置通道控制帧的通道，它们要么发生在
    // 会话建立之前，要么本来就不该重传（探活重传会污染 RTT 判定）。
    return submit_packet(dest_mac, packet, ArqLink::Reliability::Unreliable, false);
}

esp_err_t WirelessLink::submit_packet(const u8 *dest_mac, const Packet &packet, ArqLink::Reliability reliability,
                                      bool loss_injectable, u16 *seq_out, const ResponseContext &response)
{
    ArqLink::TxOptions options = {};
    options.reliable = reliability;
    options.loss_injectable = loss_injectable;

    // 序列化缓冲是对象成员：发送 helper 会被 CDC 回调、UART 桥任务、USB 任务与
    // 主循环调用，用互斥量把"序列化 + 提交"串起来。提交只入环，锁内不做等待。
    xSemaphoreTake(m_pack_mutex, portMAX_DELAY);

    const esp_err_t ret = packet.serialize(m_pack_frame, sizeof(m_pack_frame));
    if (ret != ESP_OK)
    {
        xSemaphoreGive(m_pack_mutex);
        ESP_LOGE(TAG, "Failed to serialize packet: %s", esp_err_to_name(ret));
        return ret;
    }

    const u16 len = static_cast<u16>(packet.serialized_size());
    const ArqLink::Submit submitted =
        response.is_response
            ? m_arq.submit_response(response.cmd_seq, m_pack_frame, len, dest_mac, options)
            : m_arq.submit(m_pack_frame, len, dest_mac, options, seq_out);

    xSemaphoreGive(m_pack_mutex);

    switch (submitted)
    {
    case ArqLink::Submit::Ok:
        return ESP_OK;
    case ArqLink::Submit::NoPeer:
        return ESP_ERR_NOT_FOUND;
    case ArqLink::Submit::QueueFull:
        return ESP_ERR_NO_MEM;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t WirelessLink::send_raw(const u8 *dest_mac, const u8 *data, size_t len, bool reliable,
                                 bool loss_injectable)
{
    // reliable 表示"本帧属于可靠传输机制"（数据帧、纯确认与会话重置）：它们是
    // 空口的主要用户，串口准入据此让路，撞上发送锁时也值得等。
    const u32 now = now_ms();

    if (reliable)
    {
        m_last_reliable_tx_ms = now;
    }

    // 退避窗口内不再触碰驱动。持续满速冲击 NO_MEM 会把 ESP-NOW 的发送队列带进
    // 不可恢复的状态，只有重启设备才能解开——宁可丢尽力而为的帧。
    if (!reliable && espnow_backoff_active(now))
    {
        return ESP_ERR_TIMEOUT;
    }

    if (m_tx_mutex == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(m_tx_mutex, reliable ? portMAX_DELAY : 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 注入丢帧，只对调用方声明可丢的帧生效。被丢的帧照旧计入 wl_tx_bytes：真实
    // 丢包时 esp_now_send 也是返回成功、这个计数照样加，两种丢帧因此在统计上
    // 不可区分（差别只在 send_cb 那条 ESP-NOW send failed 日志——注入不产生它，
    // 逐帧日志会扰动时序）。
    if (loss_injectable && loss_should_drop())
    {
        xSemaphoreGive(m_tx_mutex);
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(len));
        return ESP_OK;
    }
#else
    (void)loss_injectable;
#endif

    const esp_err_t send_ret = esp_now_send(dest_mac, data, len);
    xSemaphoreGive(m_tx_mutex);

    if (send_ret == ESP_ERR_ESPNOW_NO_MEM)
    {
        enter_espnow_backoff(now);
    }
    else if (send_ret == ESP_OK)
    {
        clear_espnow_backoff(now);
        // 链路层统计：成功交给 ESP-NOW 的帧（发送结果由 send_cb 另行处理，
        // 这里只反映"已发出"的字节量）
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(len));
    }
    return send_ret;
}

bool WirelessLink::uart_tx_admit(size_t len)
{
    // 链路正忙就压低串口额度，把空口让给 DAP。判据用"最近发过可靠帧"的时间戳
    // 而不是 ARQ 的 in_flight：准入跑在每一帧串口数据上，不能在那里取锁。
    const u32 now = now_ms();
    u32 bytes_per_s = CONFIG_MEGUMINLINK_UART_TX_MAX_BPS / 10;
    if ((now - m_last_reliable_tx_ms) < DAP_BUSY_WINDOW_MS)
    {
        bytes_per_s = bytes_per_s * CONFIG_MEGUMINLINK_UART_TX_MIN_PERCENT / 100;
    }

    bool admit = false;
    portENTER_CRITICAL(&m_uart_bucket_lock);
    const u32 elapsed = now - m_uart_bucket_refill_ms;
    if (elapsed > 0)
    {
        const u32 add = static_cast<u32>((static_cast<u64>(bytes_per_s) * elapsed) / 1000);
        const u32 filled = m_uart_bucket_tokens + add;
        m_uart_bucket_tokens = (filled < UART_BUCKET_CAP_BYTES) ? filled : UART_BUCKET_CAP_BYTES;
        m_uart_bucket_refill_ms = now;
    }
    if (m_uart_bucket_tokens >= len)
    {
        m_uart_bucket_tokens -= static_cast<u32>(len);
        admit = true;
    }
    portEXIT_CRITICAL(&m_uart_bucket_lock);

    if (!admit)
    {
    }
    return admit;
}

bool WirelessLink::espnow_backoff_active(u32 now) const
{
    return static_cast<i32>(m_espnow_backoff_until_ms - now) > 0;
}

void WirelessLink::enter_espnow_backoff(u32 now)
{
    const u8 level = (m_espnow_backoff_level < ESPNOW_BACKOFF_MAX_LEVEL)
                         ? static_cast<u8>(m_espnow_backoff_level + 1)
                         : ESPNOW_BACKOFF_MAX_LEVEL;
    u32 window = static_cast<u32>(ESPNOW_BACKOFF_BASE_MS) << (level - 1);
    if (window > ESPNOW_BACKOFF_MAX_MS)
    {
        window = ESPNOW_BACKOFF_MAX_MS;
    }

    const bool first_of_level = (level != m_espnow_backoff_level);
    m_espnow_backoff_level = level;
    m_espnow_backoff_until_ms = now + window;
    if (first_of_level)
    {
        ESP_LOGW(TAG, "ESP-NOW TX queue full: UART frames muted for %u ms (level %u)", static_cast<unsigned>(window),
                 level);
    }
}

void WirelessLink::clear_espnow_backoff(u32 now)
{
    if (m_espnow_backoff_level == 0 && m_espnow_backoff_until_ms == 0)
    {
        return;
    }

    // 窗口内发出成功只说明驱动队列碰巧腾出了一个空位（退避期间确认帧与 DAP 帧
    // 照常尝试），不构成"空口已恢复"的证据。窗口真正过去之后仍能发出去，才说明
    // 队列确实被排空，此时才降回正常档位。
    if (espnow_backoff_active(now))
    {
        return;
    }

    m_espnow_backoff_level = 0;
    m_espnow_backoff_until_ms = 0;
}

esp_err_t WirelessLink::send_dap_command(const u8 *dest_mac, const u8 *data, size_t len, u8 channel, u16 *seq_out)
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
    content.channel = channel;
    content.data = data;
    return submit_packet(dest_mac, packet, ArqLink::Reliability::Reliable, true, seq_out);
}

esp_err_t WirelessLink::send_dap_response(const u8 *dest_mac, const u8 *data, size_t len, u16 cmd_seq)
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

    const ResponseContext response = {true, cmd_seq};
    return submit_packet(dest_mac, packet, ArqLink::Reliability::Reliable, true, nullptr, response);
}

esp_err_t WirelessLink::send_uart_data(const u8 *data, size_t len, UartSource source)
{
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac == nullptr)
    {
        // 未配对时通常还在配对流程（PC 端不会发串口数据），DEBUG 级即可
        ESP_LOGD(TAG, "no paired peer, %u bytes dropped", static_cast<unsigned>(len));
        return ESP_ERR_NOT_FOUND;
    }

    // 帧上限是链路自己的约束：调用方按各自的读块粒度交数据（CDC 回调一次可给到
    // 256B），这里切成 MAX_UART_PAYLOAD_SIZE 的帧逐块发。中途失败即停：已经发出
    // 去的块收不回来，剩下的丢弃。
    while (len > 0)
    {
        const size_t chunk = (len < MAX_UART_PAYLOAD_SIZE) ? len : MAX_UART_PAYLOAD_SIZE;

        // 先过令牌桶再碰驱动：串口没有窗口与重传保护，放它无限灌入会把 ESP-NOW
        // 的发送队列打满，连 DAP 的可靠帧都发不出去。被拒即丢（计数在准入里）。
        if (!uart_tx_admit(chunk))
        {
            return ESP_ERR_TIMEOUT;
        }

        // Packet 为指针视图：data 指向调用方串口缓冲，零拷贝
        Packet packet;
        packet.packet_type = (source == UartSource::Host) ? PacketType::UartFromHost : PacketType::UartFromTarget;
        auto &content = packet.payload.uart_data;
        content.len = static_cast<u16>(chunk);
        content.data = data;

        const esp_err_t ret = submit_packet(peer_mac, packet, ArqLink::Reliability::Unreliable, true);
        if (ret != ESP_OK)
        {
            return ret;
        }

        data += chunk;
        len -= chunk;
    }

    return ESP_OK;
}

esp_err_t WirelessLink::send_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac == nullptr)
    {
        return ESP_ERR_NOT_FOUND;
    }

    Packet packet;
    packet.packet_type = PacketType::UartLineCoding;
    auto &content = packet.payload.uart_line_coding;
    content.baud = baud;
    content.data_bits = data_bits;
    content.stop_bits = stop_bits;
    content.parity = parity;
    // 线编码丢了就静默不跟手，值得一次重传预算
    return submit_packet(peer_mac, packet, ArqLink::Reliability::Reliable, true);
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
    // 这是一次设置下发，丢了不会有人重来：走可靠路径，且不参与注入丢帧
    // （否则丢包率调高时连"改回来"的控制请求都要靠重传才到）
    return submit_packet(dest_mac, packet, ArqLink::Reliability::Reliable, false);
}

esp_err_t WirelessLink::submit_dap_command(const u8 *cmd, size_t len, u8 channel, u16 *seq_out)
{
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac == nullptr)
    {
        ESP_LOGW(TAG, "Wireless: no paired peer, %u-byte DAP request dropped", static_cast<unsigned>(len));
        return ESP_ERR_NOT_FOUND;
    }

    // 会话握手（ArqReset）必须先完成：两端序号基准没对齐时发出去的命令会被对端
    // 当成重复帧丢掉，而对端回报的累积确认号又会把它"确认"掉，命令静默消失。
    // 握手由 ARQ 任务周期重发 + 对端回显驱动，这里只等结果。
    m_arq.set_peer(peer_mac);
    const TickType_t handshake_start = xTaskGetTickCount();
    while (!m_arq.established())
    {
        if (static_cast<u32>((xTaskGetTickCount() - handshake_start) * portTICK_PERIOD_MS) >=
            ArqLink::HANDSHAKE_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "Wireless: ARQ session not aligned within %u ms, %u-byte request dropped",
                     static_cast<unsigned>(ArqLink::HANDSHAKE_TIMEOUT_MS), static_cast<unsigned>(len));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    u16 seq = 0;
    const esp_err_t ret = send_dap_command(peer_mac, cmd, len, channel, &seq);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Wireless: cannot submit %u-byte DAP request: %s", static_cast<unsigned>(len),
                 esp_err_to_name(ret));
        return ret;
    }

    if (seq_out != nullptr)
    {
        *seq_out = seq;
    }
    ESP_LOGD(TAG, "Wireless: DAP request %uB seq=%u submitted", static_cast<unsigned>(len), seq);
    return ESP_OK;
}

bool WirelessLink::take_failed_command(u16 *seq_out)
{
    return m_arq.take_failed(seq_out);
}

bool WirelessLink::can_submit_command()
{
    return m_arq.can_submit();
}

// ---- 无线侧 DAP 执行接线 ----
// 与 usb.cc 的 dap_task 对称：无线传输层负责把收到的 DapCommand 交给公共
// 执行器（dap_executor_run）执行，并把结果以 DapResponse 帧回发给来源。
// 执行循环、DAP_Setup 与命令日志均在 dap_executor.cc，这里只有队列、
// 任务与输入/输出回调；状态全在 WirelessLink 对象内。

bool WirelessLink::dap_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);

    // 落点是对象成员而不是局部变量：这条命令约 1.4 KB，放在栈上会占掉 4096
    // 字节任务栈的三分之一，而下面还要跑 DAP_ExecuteCommand 的整条调用链。
    // 本函数只由 dap-wireless 任务调用，单线程访问该成员。
    DapExecuteRequest &req = link->m_dap_pending;
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

    // DAP_Info 上报的单包尺寸按命令来源通道给：HID 是定长 64B 报告，只有 vendor
    // bulk 才用得上 1274B 这个安全上限（见 DAP_REPORTED_PACKET_SIZE_MAX）。混用会
    // 让主机按 1274 构造装不进 64B 报告的命令——那正是 DAP_TransferBlock 被截断、
    // 执行器判长度不足而回 Invalid 的原因。
    DAP_Data.packet_size = (req.channel == DAP_CHANNEL_HID) ? DAP_HID_PACKET_SIZE
                                                            : static_cast<u16>(DAP_REPORTED_PACKET_SIZE_MAX);
    return true;
}

void WirelessLink::dap_resp_sink(const u8 *data, u16 len, void *arg)
{
    auto *link = static_cast<WirelessLink *>(arg);

    // 响应走可靠传输，序号与确认由 ARQ 层给，并记下它回的是哪条命令——对端重传
    // 那条命令时据此重放这条响应，而不是把命令再执行一遍。窗口满不会丢响应：
    // 它在 ARQ 层的待发环里等窗口空位。
    const esp_err_t ret = link->send_dap_response(link->m_dap_src_mac, data, len, link->m_dap_cmd_seq);
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

esp_err_t WirelessLink::dap_execute(const u8 *src_mac, u16 cmd_seq, const u8 *data, size_t len, u8 channel)
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
    req.channel = channel;
    std::memcpy(req.data, data, len);

    if (xQueueSend(m_dap_queue, &req, 0) != pdTRUE)
    {
        m_cmd_queue_dropped = m_cmd_queue_dropped + 1;
        ESP_LOGW(TAG, "DAP command queue full, command dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

u32 WirelessLink::cmd_queue_dropped() const
{
    return m_cmd_queue_dropped;
}

// ---- ARQ 接线 ----
// ARQ 层自带任务，本层与它之间只有两件事：把一帧缓冲发出去（arq_send_raw），
// 以及把一条到达的载荷交给业务（arq_deliver）。序号、窗口、重传与确认全在那一层，
// 本层不参与——它只知道"这是什么业务包"。

bool WirelessLink::arq_established() const
{
    return m_arq.established();
}

ArqLink::Snapshot WirelessLink::arq_snapshot() const
{
    return m_arq.snapshot();
}

ArqSettings WirelessLink::arq_settings() const
{
    return m_arq.settings();
}

esp_err_t WirelessLink::arq_apply_settings(const ArqSettings &settings)
{
    return m_arq.apply_settings(settings);
}

bool WirelessLink::arq_take_settings_dirty()
{
    return m_arq.take_settings_dirty();
}

void WirelessLink::notify_link_alive()
{
    m_arq.request_link_alive();
}

void WirelessLink::arq_send_raw(void *ctx, const u8 *dest_mac, const u8 *data, size_t len, bool reliable,
                                bool loss_injectable)
{
    auto *self = static_cast<WirelessLink *>(ctx);
    const esp_err_t ret = self->send_raw(dest_mac, data, len, reliable, loss_injectable);
    if (ret != ESP_OK && reliable)
    {
        ESP_LOGW(TAG, "Failed to send reliable frame to " MACSTR ": %s", MAC2STR(dest_mac), esp_err_to_name(ret));
    }
}

void WirelessLink::arq_deliver(void *ctx, const u8 *src_mac, u16 seq, const u8 *payload, u16 len)
{
    auto *self = static_cast<WirelessLink *>(ctx);
    auto &app = Application::get_instance();

    // 反序列化落在对象成员上：数据段可达 MAX_DAP_PAYLOAD_SIZE，放栈上会吃掉 ARQ
    // 任务的栈。本函数是这条路径的唯一使用者（任务上下文单线程）。
    Packet packet;
    if (packet.deserialize(payload, len, self->m_rx_storage, sizeof(self->m_rx_storage)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to deserialize %u-byte payload from " MACSTR, static_cast<unsigned>(len),
                 MAC2STR(src_mac));
        return;
    }

    // 交付回调必须只做非阻塞的事：它跑在 ARQ 任务里，重活会推迟重传定时器。
    // 需要碰驱动的操作（速率、线编码）因此只登记，留给主循环 poll()。
    switch (packet.packet_type)
    {
    case PacketType::DapCommand: {
        // 命令直通投递（不经主循环）。A/B 实测（2026-08-15）：这是最大收益项——
        // 接收端（C3，单核）原来每帧要经主循环中转，而 esp_wifi 任务收帧后持续
        // 占用 CPU，主循环迟迟得不到调度，每命令多 ~9ms。
        const auto &cmd = packet.payload.dap_command;
        const esp_err_t ret = self->dap_execute(src_mac, seq, cmd.data, cmd.len, cmd.channel);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to enqueue DAP command from " MACSTR ": %s", MAC2STR(src_mac),
                     esp_err_to_name(ret));
        }
        return;
    }
    case PacketType::DapResponse: {
        // 响应交给 USB 转发路径：序号带上，USB 侧按它配回挂起的命令
        const auto &resp = packet.payload.dap_response;
        app.push_wireless_response(seq, resp.data, resp.len);
        return;
    }
    case PacketType::UartFromHost: {
        // 方向由包类型承载——来自主机侧的写本机 UART TX。只做一次拷贝入队，真正的
        // 写入（写 UART 会阻塞）留给桥的 tx 任务。
        const auto &uart = packet.payload.uart_data;
        app.forward_uart_data_from_wireless(uart.data, uart.len, UartSource::Host);
        return;
    }
    case PacketType::UartFromTarget: {
        const auto &uart = packet.payload.uart_data;
        app.forward_uart_data_from_wireless(uart.data, uart.len, UartSource::Target);
        return;
    }
    case PacketType::UartLineCoding: {
        // configure 会调 UART 驱动，不能占着 ARQ 任务的重传定时器：登记给主循环
        const auto &lc = packet.payload.uart_line_coding;
        portENTER_CRITICAL(&self->m_line_coding_lock);
        self->m_pending_line_coding.baud = lc.baud;
        self->m_pending_line_coding.data_bits = lc.data_bits;
        self->m_pending_line_coding.stop_bits = lc.stop_bits;
        self->m_pending_line_coding.parity = lc.parity;
        self->m_line_coding_pending = true;
        portEXIT_CRITICAL(&self->m_line_coding_lock);
        return;
    }
    case PacketType::RelayedUsbControl: {
        // 对端转发的 USB 控制请求（无 USB 侧的设置下发通路）：与 USB 侧同一张命令表，
        // 这里只解包投递，执行在主循环——命令可能操作 WiFi 驱动，不能在交付路径里重入。
        const auto &relay = packet.payload.relayed_usb_control;
        app.config_channel().on_relayed_control(relay);
        return;
    }
    default: {
        // 配对控制帧与 Ping/Pong：维持经主循环统一驱动状态机的原路径
        ReceivedPacket rp = {};
        std::memcpy(rp.src_mac, src_mac, ESP_NOW_ETH_ALEN);
        rp.packet = packet;
        app.push_received_packet(rp);
        return;
    }
    }
}
