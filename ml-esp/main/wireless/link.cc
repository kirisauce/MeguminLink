#include "meguminlink/wireless/link.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp" // dap_executor_run（无线侧执行接线）
#include "meguminlink/dap_frame.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless/espnow_transport.hpp"

#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 dap_executor.cc 一致）。
// clang-format off
#include "DAP_config.h"
#include "DAP.h"
// clang-format on

using namespace mlink;

static constexpr char TAG[] = "mlink::wireless_link";

namespace
{
} // namespace

bool mlink::wifi_rate_supported(u8 rate)
{
    return EspNowTransport::rate_supported(rate);
}

// ---- Transport 回调（C 函数指针，实例经 ctx 传入） ----

void WirelessLink::on_frame(void *ctx, const u8 *src_mac, const u8 *data, usize len)
{
    static_cast<WirelessLink *>(ctx)->handle_frame(src_mac, data, len);
}

void WirelessLink::on_rx_stats(void *, u16 bytes, i8 rssi)
{
    // 链路层统计：收到的每一帧都计（含被判为畸形的帧——它们同样占用了空口与
    // ESP-NOW 任务），RSSI 随帧更新
    Application::get_instance().config_channel().add_wireless_rx(bytes, rssi);
}

void WirelessLink::on_tx_result(void *ctx, const u8 *, bool ok)
{
    static_cast<WirelessLink *>(ctx)->handle_tx_result(ok);
}

void WirelessLink::handle_frame(const u8 *src_mac, const u8 *data, usize len)
{
    // 反序列化落在对象成员上：数据段可达 MAX_DAP_PAYLOAD_SIZE，放栈上会吃掉
    // 任务的栈。本函数是这条路径的唯一使用者（任务上下文单线程）。
    Packet packet;
    if (packet.deserialize(data, len, m_rx_storage, sizeof(m_rx_storage)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to deserialize %u-byte payload from " MACSTR, static_cast<unsigned>(len),
                 MAC2STR(src_mac));
        return;
    }

    // 交付回调必须只做非阻塞的事：它跑在 esp_wifi 任务里
    switch (packet.packet_type)
    {
    case PacketType::DapCommand:
    case PacketType::DapResponse:
    case PacketType::UartFromHost:
    case PacketType::UartFromTarget:
    case PacketType::UartLineCoding:
    case PacketType::RelayedUsbControl:
        // 业务类型已改走 meaproto 通道：出现在链路层属异常，忽略
        return;
    default: {
        // 配对控制帧与 Ping/Pong：经主循环驱动状态机
        ReceivedPacket rp = {};
        std::memcpy(rp.src_mac, src_mac, ESP_NOW_ETH_ALEN);
        rp.packet = packet;
        Application::get_instance().push_received_packet(rp);
        return;
    }
    }
}

void WirelessLink::handle_tx_result(bool ok)
{
    if (ok)
    {
        m_send_fail_streak = 0;
        return;
    }

    const u32 now = now_ms();
    const u8 streak = (m_send_fail_streak < 0xFF) ? static_cast<u8>(m_send_fail_streak + 1) : 0xFF;
    m_send_fail_streak = streak;

    // 连续失败达到阈值：与 NO_MEM 共用同一套退避
    if (streak >= ESPNOW_TX_FAIL_STREAK_THRESHOLD)
    {
        m_send_fail_streak = 0; // 重新累计：持续失败期间由退避窗口持续刷新
        enter_espnow_backoff(now);
    }

    // 失败日志限速：差链路下每帧一条会把日志刷满
    if (static_cast<i32>(now - m_send_fail_log_ms) >= 0)
    {
        m_send_fail_log_ms = now + ESPNOW_TX_FAIL_LOG_INTERVAL_MS;
        ESP_LOGW(TAG, "ESP-NOW send failed (streak %u)", streak);
    }
}

WirelessLink::WirelessLink() : m_daemon(EspNowTransport::instance(), *this, channels(), LINK_CONFIG)
{
}

std::span<const meaproto::ChannelSpec> WirelessLink::channels()
{
    return CHANNELS;
}

bool WirelessLink::connected() const
{
    return m_daemon.connected();
}

bool WirelessLink::connecting() const
{
    return m_daemon.connecting();
}

void WirelessLink::on_message(meaproto::ChannelId channel, std::span<const u8> data)
{
    if (channel == DAP_CHANNEL)
    {
        handle_dap_message(data);
        return;
    }
    if (channel == UART_CHANNEL)
    {
        handle_uart_message(data);
        return;
    }

    ESP_LOGD(TAG, "channel %u message (%u bytes) not dispatched", static_cast<unsigned>(channel.value),
             static_cast<unsigned>(data.size()));
}

void WirelessLink::handle_dap_message(std::span<const u8> data)
{
    if (data.size() < DAP_WIRE_HEADER)
    {
        ESP_LOGW(TAG, "DAP message too short (%u bytes)", static_cast<unsigned>(data.size()));
        return;
    }

    const u8 kind = data[0];
    const u8 source = data[1];
    const u16 req_id = static_cast<u16>((data[2] << 8) | data[3]);
    const std::span<const u8> body = data.subspan(DAP_WIRE_HEADER);

    if (kind == DAP_KIND_RESPONSE)
    {
        // 交付回调只许拷贝入队：响应由 USB 任务取走后写回主机
        Application::get_instance().push_wireless_response(req_id, body.data(), static_cast<u16>(body.size()));
        return;
    }

    if (m_dap_queue == nullptr || body.size() > sizeof(m_dap_post.data))
    {
        ESP_LOGW(TAG, "DAP command dropped (%u bytes)", static_cast<unsigned>(body.size()));
        return;
    }

    m_dap_post.source = source;
    m_dap_post.req_id = req_id;
    m_dap_post.len = static_cast<u16>(body.size());
    std::memcpy(m_dap_post.data, body.data(), body.size());

    if (xQueueSend(m_dap_queue, &m_dap_post, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "DAP execute queue full, cmd 0x%02X dropped", m_dap_post.data[0]);
    }
}

meaproto::SendResult WirelessLink::send_dap_frame(u8 kind, u8 source, u16 req_id, const u8 *data, usize len, u8 *frame)
{
    if (len > DAP_CHANNEL_PAYLOAD - DAP_WIRE_HEADER)
    {
        m_dap_too_large = m_dap_too_large + 1;
        ESP_LOGW(TAG, "DAP payload %u exceeds channel limit %u (dropped %u)", static_cast<unsigned>(len),
                 static_cast<unsigned>(DAP_CHANNEL_PAYLOAD - DAP_WIRE_HEADER), static_cast<unsigned>(m_dap_too_large));
        return meaproto::SendResult::TooLarge;
    }

    frame[0] = kind;
    frame[1] = source;
    frame[2] = static_cast<u8>(req_id >> 8);
    frame[3] = static_cast<u8>(req_id & 0xFF);
    std::memcpy(frame + DAP_WIRE_HEADER, data, len);

    return m_daemon.send(DAP_CHANNEL, std::span<const u8>{frame, DAP_WIRE_HEADER + len});
}

meaproto::SendResult WirelessLink::send_dap_command(const u8 *data, usize len, u8 source, u16 req_id)
{
    return send_dap_frame(DAP_KIND_COMMAND, source, req_id, data, len, m_dap_cmd_frame);
}

meaproto::SendResult WirelessLink::send_dap_response(const u8 *data, usize len, u16 req_id)
{
    return send_dap_frame(DAP_KIND_RESPONSE, 0, req_id, data, len, m_dap_rsp_frame);
}

bool WirelessLink::can_send_dap() const
{
    // 连接状态必须一起判：未连接时 Core 的队列照样有空位
    return m_daemon.connected() && m_daemon.can_send(DAP_CHANNEL);
}

u32 WirelessLink::disconnect_generation() const
{
    return m_disconnect_generation.load(std::memory_order_relaxed);
}

void WirelessLink::dap_task_fn(void *arg)
{
    auto *self = static_cast<WirelessLink *>(arg);
    DAP_Setup();

    while (true)
    {
        if (xQueueReceive(self->m_dap_queue, &self->m_dap_exec, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        DapExecuteRequest &request = self->m_dap_exec;
        // 单包尺寸按主机的传输通道选：HID 是定长报告，vendor bulk 受 CMSIS-DAP
        // 单字节 Transfer Count 的安全上限约束
        DAP_Data.packet_size = (request.source == DAP_CHANNEL_VENDOR_BULK)
                                   ? static_cast<u16>(DAP_REPORTED_PACKET_SIZE_MAX)
                                   : DAP_HID_PACKET_SIZE;
        const u16 resp_len =
            dap_execute_once(request.data, request.len, self->m_dap_response, sizeof(self->m_dap_response));
        (void)self->send_dap_response(self->m_dap_response, resp_len, request.req_id);
    }
}

void WirelessLink::on_connected()
{
    ESP_LOGI(TAG, "meaproto connected");
}

void WirelessLink::on_disconnected(meaproto::DisconnectReason reason)
{
    m_disconnect_generation.fetch_add(1, std::memory_order_relaxed);
    flush_uart_egress();
    ESP_LOGW(TAG, "meaproto disconnected (reason %u)", static_cast<unsigned>(reason));
}

void WirelessLink::handle_uart_message(std::span<const u8> data)
{
    if (data.empty())
    {
        return;
    }

    const u8 source = data[0];
    const std::span<const u8> body = data.subspan(1);

    if (source == UART_SOURCE_LINE_CODING)
    {
        if (body.size() != UART_LINE_CODING_SIZE)
        {
            ESP_LOGW(TAG, "Line coding payload of %u bytes, dropped", static_cast<unsigned>(body.size()));
            return;
        }
        const u32 baud = (static_cast<u32>(body[0]) << 24) | (static_cast<u32>(body[1]) << 16) |
                         (static_cast<u32>(body[2]) << 8) | static_cast<u32>(body[3]);
        m_line_baud = baud;
        Application::get_instance().uart_bridge().configure(baud, body[4], body[5], body[6]);
        return;
    }

    if (body.empty())
    {
        return;
    }
    const UartSource port = (source == UART_SOURCE_HOST) ? UartSource::Host : UartSource::Target;
    Application::get_instance().uart_bridge().push_inbound(body.data(), body.size(), port);
}

size_t WirelessLink::send_uart_data(const u8 *data, size_t len, UartSource source)
{
    UartEgress &egress = uart_egress(source);
    size_t done = 0;

    // 缓冲满了就等一等，让空口追上；等不到的部分丢弃，由调用方记账
    for (u32 waited = 0; done < len && waited <= UART_PUSH_WAIT_MS; waited += UART_PUSH_WAIT_STEP_MS)
    {
        done += egress.push(data + done, len - done);
        if (done == len)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(UART_PUSH_WAIT_STEP_MS));
    }

    if (done > 0)
    {
        notify_uart_egress();
    }
    return done;
}

size_t WirelessLink::uart_egress_space(UartSource source)
{
    return uart_egress(source).space();
}

size_t WirelessLink::push_uart_egress(const u8 *data, size_t len, UartSource source)
{
    return uart_egress(source).push(data, len);
}

void WirelessLink::flush_uart_egress()
{
    m_host_uart.flush();
    m_target_uart.flush();
}

void WirelessLink::notify_uart_egress()
{
    xSemaphoreGive(Application::get_instance().main_task_wake());
}

void WirelessLink::set_host_uart_fill(HostUartFill fill, void *ctx)
{
    m_host_uart_fill = fill;
    m_host_uart_fill_ctx = ctx;
}

bool WirelessLink::poll_uart_egress()
{
    // 先补读：缓冲腾出空间后把 CDC 端点重新打开
    if (m_host_uart_fill != nullptr)
    {
        m_host_uart_fill(m_host_uart_fill_ctx);
    }

    // 没有配对就没有落点
    if (peer_mac_key().cached() == nullptr)
    {
        flush_uart_egress();
        return false;
    }

    // 目标方向先排：它是对端串口回给主机的数据，卡住会把主机侧拖成超时
    drain_uart_egress(m_target_uart, UartSource::Target);
    drain_uart_egress(m_host_uart, UartSource::Host);
    return (m_target_uart.size() + m_host_uart.size()) > 0;
}

WirelessLink::UartVerdict WirelessLink::uart_frame_verdict() const
{
    if (!m_daemon.connected())
    {
        return UartVerdict::Wait;
    }
    return m_daemon.can_send(UART_CHANNEL) ? UartVerdict::Send : UartVerdict::Wait;
}

u32 WirelessLink::uart_bytes_per_s() const
{
    const u32 baud = (m_line_baud != 0) ? m_line_baud : CONFIG_MEGUMINLINK_UART_TX_MAX_BPS;
    return baud / 10;
}

bool WirelessLink::uart_tx_admit(usize len, UartSource source)
{
    // 最近提交过 DAP 业务就把串口额度压下来，把空口让给 DAP
    const u32 now = now_ms();
    u32 bytes_per_s = uart_bytes_per_s();
    if ((now - m_last_dap_tx_ms) < DAP_BUSY_WINDOW_MS)
    {
        bytes_per_s = bytes_per_s * CONFIG_MEGUMINLINK_UART_TX_MIN_PERCENT / 100;
    }

    UartBucket &bucket = (source == UartSource::Host) ? m_uart_bucket_host : m_uart_bucket_target;
    bool admit = false;
    portENTER_CRITICAL(&bucket.lock);
    const u32 elapsed = now - bucket.refill_ms;
    if (elapsed > 0)
    {
        const u32 add = static_cast<u32>((static_cast<u64>(bytes_per_s) * elapsed) / 1000);
        const u32 filled = bucket.tokens + add;
        bucket.tokens = (filled < UART_BUCKET_CAP_BYTES) ? filled : UART_BUCKET_CAP_BYTES;
        bucket.refill_ms = now;
    }
    if (bucket.tokens >= len)
    {
        bucket.tokens -= static_cast<u32>(len);
        admit = true;
    }
    portEXIT_CRITICAL(&bucket.lock);
    return admit;
}

void WirelessLink::drain_uart_egress(UartEgress &egress, UartSource source)
{
    while (true)
    {
        if (uart_frame_verdict() != UartVerdict::Send)
        {
            break;
        }

        const usize n = egress.peek(m_uart_frame + 1, UART_CHUNK);
        if (n == 0)
        {
            break;
        }
        if (!uart_tx_admit(n, source))
        {
            break;
        }

        m_uart_frame[0] = (source == UartSource::Host) ? UART_SOURCE_HOST : UART_SOURCE_TARGET;
        if (m_daemon.send(UART_CHANNEL, std::span<const u8>{m_uart_frame, n + 1}) != meaproto::SendResult::Ok)
        {
            break; // 这一帧留在缓冲里，下一轮再来
        }

        egress.consume(n);
        Application::get_instance().config_channel().add_uart_wireless_tx(static_cast<u16>(n));
    }
}

UartEgress &WirelessLink::uart_egress(UartSource source)
{
    return (source == UartSource::Host) ? m_host_uart : m_target_uart;
}

meaproto::SendResult WirelessLink::send_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    if (baud != 0)
    {
        m_line_baud = baud;
    }

    u8 frame[1 + UART_LINE_CODING_SIZE] = {};
    frame[0] = UART_SOURCE_LINE_CODING;
    frame[1] = static_cast<u8>(baud >> 24);
    frame[2] = static_cast<u8>(baud >> 16);
    frame[3] = static_cast<u8>(baud >> 8);
    frame[4] = static_cast<u8>(baud);
    frame[5] = data_bits;
    frame[6] = stop_bits;
    frame[7] = parity;
    return m_daemon.send(UART_CHANNEL, std::span<const u8>{frame, sizeof(frame)});
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

    m_dap_queue = xQueueCreate(DAP_EXEC_QUEUE_DEPTH, sizeof(DapExecuteRequest));
    if (m_dap_queue == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create wireless DAP queue");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 注入丢帧的随机序列：两端各自用 esp_random 播种，不共享序列——固定种子会
    // 让两台设备在相同帧号上同时丢帧，制造出现实里不存在的人为相关
    m_loss_rng = esp_random() | 1u;
#endif

    esp_err_t ret;
    EspNowTransport &transport = EspNowTransport::instance();
    ret = transport.init();
    ESP_RETURN_ON_ERROR(ret, TAG, "EspNowTransport::init() failed");

    transport.set_non_meaproto_frame_callback(&WirelessLink::on_frame, this);
    transport.set_rx_observer_callback(&WirelessLink::on_rx_stats, this);
    transport.set_tx_result_callback(&WirelessLink::on_tx_result, this);

    constexpr unsigned channel_count = static_cast<unsigned>(sizeof(CHANNELS) / sizeof(CHANNELS[0]));
    const usize storage_size = meaproto::Core::storage_bytes(channels(), LINK_CONFIG, EspNowTransport::MTU);
    ESP_LOGI(TAG, "channels %u, protocol storage %u bytes", channel_count, static_cast<unsigned>(storage_size));

    // 执行任务的优先级与本地 DAP 执行器一致：SWD 位碰撞期间不可被抢占
    if (xTaskCreate(dap_task_fn, "dap-wireless", 8192, this, 24, &m_dap_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create wireless DAP task");
        return ESP_ERR_NO_MEM;
    }

    m_daemon.start();

    m_initialized = true;
    return ESP_OK;
}

esp_err_t WirelessLink::peer_add(const u8 *peer_mac)
{
    EspNowTransport &transport = EspNowTransport::instance();
    const esp_err_t ret = transport.peer_add(peer_mac);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // 重复调用是幂等的；速率配置在 peer 存在后总是重新下发给驱动
    return transport.set_peer_rate(peer_mac, m_rate);
}

esp_err_t WirelessLink::peer_remove(const u8 *peer_mac)
{
    return EspNowTransport::instance().peer_remove(peer_mac);
}

esp_err_t WirelessLink::set_peer_rate(const u8 *peer_mac)
{
    return EspNowTransport::instance().set_peer_rate(peer_mac, m_rate);
}

u8 WirelessLink::wifi_rate() const
{
    return m_rate;
}

esp_err_t WirelessLink::set_wifi_rate(u8 rate)
{
    if (!EspNowTransport::rate_supported(rate))
    {
        ESP_LOGW(TAG, "Unsupported WiFi rate 0x%02X", static_cast<unsigned>(rate));
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

    ESP_LOGI(TAG, "WiFi rate set to 0x%02X", static_cast<unsigned>(rate));
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
    // 只做概率判定，被丢的帧由调用方记账
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
    // 0) 配对状态收敛到协议连接：已配对则登记 peer、绑定传输并发起握手；未配对
    //    则断开并把传输解绑。两步都幂等。
    EspNowTransport &transport = EspNowTransport::instance();
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac == nullptr)
    {
        m_daemon.close();
        transport.unbind();
    }
    else
    {
        (void)peer_add(peer_mac);
        if (!transport.bound_to(peer_mac))
        {
            (void)transport.bind(peer_mac);
        }
        m_daemon.open();
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
}

esp_err_t WirelessLink::send(const u8 *dest_mac, const Packet &packet)
{
    // 序列化缓冲是对象成员：发送 helper 会被 CDC 回调、USB 任务与主循环调用，
    // 用互斥量把序列化串起来。
    xSemaphoreTake(m_pack_mutex, portMAX_DELAY);

    const esp_err_t ret = packet.serialize(m_pack_frame, sizeof(m_pack_frame));
    if (ret != ESP_OK)
    {
        xSemaphoreGive(m_pack_mutex);
        ESP_LOGE(TAG, "Failed to serialize packet: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_err_t send_ret = send_frame(dest_mac, m_pack_frame, packet.serialized_size());
    xSemaphoreGive(m_pack_mutex);
    return send_ret;
}

esp_err_t WirelessLink::send_frame(const u8 *dest_mac, const u8 *data, size_t len)
{
    const u32 now = now_ms();

    // 退避窗口内不再触碰驱动。持续满速冲击会把 ESP-NOW 的发送队列带进不可恢复
    // 的状态（NO_MEM，或对端持续不应答），只有重启设备才能解开——宁可丢帧。
    if (espnow_backoff_active(now))
    {
        return ESP_ERR_TIMEOUT;
    }

    if (m_tx_mutex == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(m_tx_mutex, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 注入丢帧：被丢的帧照旧计入 wl_tx_bytes，真实丢包时底层发送也返回成功、
    // 这个计数照样加，两种丢帧在统计上不可区分。
    if (loss_should_drop())
    {
        xSemaphoreGive(m_tx_mutex);
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(len));
        return ESP_OK;
    }
#endif

    const esp_err_t send_ret = EspNowTransport::instance().send_to(dest_mac, std::span<const u8>{data, len});
    xSemaphoreGive(m_tx_mutex);

    if (send_ret == ESP_ERR_ESPNOW_NO_MEM)
    {
        enter_espnow_backoff(now);
    }
    else if (send_ret == ESP_OK)
    {
        clear_espnow_backoff(now);
        // 链路层统计：成功交给 ESP-NOW 的帧（发送结果由 tx 结果回调另行处理，
        // 这里只反映"已发出"的字节量）
        Application::get_instance().config_channel().add_wireless_tx(static_cast<u16>(len));
    }
    return send_ret;
}

bool WirelessLink::espnow_backoff_active(u32 now) const
{
    return static_cast<i32>(m_espnow_backoff_until_ms - now) > 0;
}

void WirelessLink::enter_espnow_backoff(u32 now)
{
    u8 level = 0;
    u32 window = 0;
    bool first_of_level = false;

    portENTER_CRITICAL(&m_espnow_backoff_lock);
    level = (m_espnow_backoff_level < ESPNOW_BACKOFF_MAX_LEVEL) ? static_cast<u8>(m_espnow_backoff_level + 1)
                                                                : ESPNOW_BACKOFF_MAX_LEVEL;
    window = static_cast<u32>(ESPNOW_BACKOFF_BASE_MS) << (level - 1);
    if (window > ESPNOW_BACKOFF_MAX_MS)
    {
        window = ESPNOW_BACKOFF_MAX_MS;
    }
    first_of_level = (level != m_espnow_backoff_level);
    m_espnow_backoff_level = level;
    m_espnow_backoff_until_ms = now + window;
    portEXIT_CRITICAL(&m_espnow_backoff_lock);

    // 日志在临界区外：只有档位真的抬升才打一条，持续失败期间不会重复刷
    if (first_of_level)
    {
        ESP_LOGW(TAG, "ESP-NOW TX backoff: best-effort frames muted for %u ms (level %u)",
                 static_cast<unsigned>(window), level);
    }
}

void WirelessLink::clear_espnow_backoff(u32 now)
{
    portENTER_CRITICAL(&m_espnow_backoff_lock);
    const bool pending = (m_espnow_backoff_level != 0 || m_espnow_backoff_until_ms != 0);
    // 窗口内发出成功只说明驱动队列碰巧腾出了一个空位（退避期间确认帧与 DAP 帧
    // 照常尝试），不构成"空口已恢复"的证据。窗口真正过去之后仍能发出去，才说明
    // 队列确实被排空，此时才降回正常档位。
    const bool expired = !(static_cast<i32>(m_espnow_backoff_until_ms - now) > 0);
    if (pending && expired)
    {
        m_espnow_backoff_level = 0;
        m_espnow_backoff_until_ms = 0;
    }
    portEXIT_CRITICAL(&m_espnow_backoff_lock);
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

bool WirelessLink::arq_established() const
{
    return false;
}

ArqLink::Snapshot WirelessLink::arq_snapshot() const
{
    return {};
}

ArqSettings WirelessLink::arq_settings() const
{
    return m_settings;
}

esp_err_t WirelessLink::arq_apply_settings(const ArqSettings &settings)
{
    m_settings = settings;
    return ESP_OK;
}

bool WirelessLink::arq_take_settings_dirty()
{
    return false;
}
