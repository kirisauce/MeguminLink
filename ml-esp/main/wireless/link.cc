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

    // 交付回调必须只做非阻塞的事：它跑在 esp_wifi 任务里。需要碰驱动的操作
    //（速率、线编码）因此只登记，留给主循环 poll()。
    switch (packet.packet_type)
    {
    case PacketType::DapCommand:
        // 业务窗口：DAP 转发不可用
        return;
    case PacketType::DapResponse: {
        const auto &resp = packet.payload.dap_response;
        Application::get_instance().push_wireless_response(resp.req_id, resp.data, resp.len);
        return;
    }
    case PacketType::UartFromHost:
    case PacketType::UartFromTarget: {
        const auto &uart = packet.payload.uart_data;
        const UartSource source =
            (packet.packet_type == PacketType::UartFromHost) ? UartSource::Host : UartSource::Target;
        Application::get_instance().forward_uart_data_from_wireless(uart.data, uart.len, source);
        return;
    }
    case PacketType::UartLineCoding: {
        const auto &lc = packet.payload.uart_line_coding;
        portENTER_CRITICAL(&m_line_coding_lock);
        m_pending_line_coding.baud = lc.baud;
        m_pending_line_coding.data_bits = lc.data_bits;
        m_pending_line_coding.stop_bits = lc.stop_bits;
        m_pending_line_coding.parity = lc.parity;
        m_line_coding_pending = true;
        portEXIT_CRITICAL(&m_line_coding_lock);
        return;
    }
    case PacketType::RelayedUsbControl: {
        const auto &relay = packet.payload.relayed_usb_control;
        Application::get_instance().config_channel().on_relayed_control(relay);
        return;
    }
    default: {
        // 配对控制帧与 Ping/Pong：维持经主循环统一驱动状态机的原路径
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
    // 0) 配对信息交给 ESP-NOW：单播 peer 必须先登记进 peer list，否则手上的帧
    //    一律发送失败。幂等（只有真的换了对端才会变化）；执行侧（Local 模式）同样
    //    需要——它不发起命令，但要能回应交互。
    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac != nullptr)
    {
        (void)peer_add(peer_mac);
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

    // 2) 交付路径登记下来的线编码参数：configure 会调 UART 驱动，只登记、在这里应用。
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

    // 3) 串口出向缓冲
    poll_uart_egress();
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

bool WirelessLink::uart_tx_admit(size_t len, UartSource source)
{
    // 链路正忙就压低串口额度，把空口让给 DAP。判据是"最近提交过 DAP 业务包"的
    // 时间戳：准入跑在每一帧串口数据上，不能在那里取锁。
    const u32 now = now_ms();
    u32 bytes_per_s = CONFIG_MEGUMINLINK_UART_TX_MAX_BPS / 10;
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

esp_err_t WirelessLink::send_dap_command(const u8 *, const u8 *, size_t, u8, u16, u16 *)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t WirelessLink::send_dap_response(const u8 *, const u8 *, size_t, u16, u16)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t WirelessLink::send_uart_frame(const u8 *, size_t, UartSource, ArqLink::Reliability)
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t WirelessLink::send_uart_data(const u8 *data, size_t len, UartSource source)
{
    // 调用方必须是能睡的端口任务（UART 桥）：排在缓冲里的字节由主循环排空。
    UartEgress &egress = uart_egress(source);
    size_t done = 0;

    // 目标一侧没有流控（CDC 那侧靠端点 NAK 反压）：缓冲满了就等一等，让空口追上，
    // 等不到的才丢——屏幕回包里的确认丢一条就会让主机侧白等到超时。
    for (u32 waited_ms = 0; done < len && waited_ms <= UART_PUSH_WAIT_MS; waited_ms += UART_PUSH_WAIT_STEP_MS)
    {
        done += egress.push(data + done, len - done);
        if (done == len)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(UART_PUSH_WAIT_STEP_MS));
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

void WirelessLink::set_host_uart_fill(HostUartFill fill, void *ctx)
{
    m_host_uart_fill = fill;
    m_host_uart_fill_ctx = ctx;
}

void WirelessLink::notify_uart_egress()
{
}

bool WirelessLink::uart_reliable() const
{
    return m_uart_reliable;
}

void WirelessLink::set_uart_reliable(bool reliable)
{
    m_uart_reliable = reliable;
    ESP_LOGI(TAG, "UART egress reliability %s", reliable ? "on" : "off");
}

UartEgress &WirelessLink::uart_egress(UartSource source)
{
    return (source == UartSource::Host) ? m_host_uart : m_target_uart;
}

void WirelessLink::poll_uart_egress()
{
    // 先补读：缓冲腾出空间后把 CDC 端点重新打开
    if (m_host_uart_fill != nullptr)
    {
        m_host_uart_fill(m_host_uart_fill_ctx);
    }

    // 没有配对就没有落点：丢掉两条缓冲，重新配对后从新字节开始
    if (peer_mac_key().cached() == nullptr)
    {
        flush_uart_egress();
        return;
    }

    // 目标方向先排：它是对端串口回给主机的确认与数据，卡住它等于把主机侧拖成超时，
    // 而主机方向的下载突发本来就有缓冲兜着
    drain_uart_egress(m_target_uart, UartSource::Target);
    drain_uart_egress(m_host_uart, UartSource::Host);
}

WirelessLink::UartVerdict WirelessLink::uart_frame_verdict() const
{
    return UartVerdict::Unreliable;
}

void WirelessLink::drain_uart_egress(UartEgress &egress, UartSource source)
{
    u8 frame[MAX_UART_PAYLOAD_SIZE];

    while (true)
    {
        const size_t n = egress.peek(frame, sizeof(frame));
        if (n == 0)
        {
            break;
        }

        const UartVerdict verdict = uart_frame_verdict();
        if (verdict == UartVerdict::Wait)
        {
            // 让路只是暂缓：这一帧留在缓冲里，下一轮再看，满了照样反压主机
            break;
        }
        const ArqLink::Reliability reliability =
            (verdict == UartVerdict::Reliable) ? ArqLink::Reliability::Reliable : ArqLink::Reliability::Unreliable;

        // 额度不足或驱动暂时发不出去：这一帧留在缓冲里，下一轮接着来
        if (!uart_tx_admit(n, source))
        {
            break;
        }
        if (send_uart_frame(frame, n, source, reliability) != ESP_OK)
        {
            break;
        }

        egress.consume(n);
    }
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
    return send(peer_mac, packet);
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

esp_err_t WirelessLink::submit_dap_command(const u8 *, size_t, u8, u16, u16 *)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool WirelessLink::take_failed_command(u16 *)
{
    return false;
}

bool WirelessLink::can_submit_command()
{
    return false;
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
    link->m_dap_req_id = req.req_id;

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

    // 响应按最近一条命令的请求号回发；DAP 窗口关闭期间这里只会拿到不支持。
    const esp_err_t ret =
        link->send_dap_response(link->m_dap_src_mac, data, len, link->m_dap_req_id, link->m_dap_cmd_seq);
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

u32 WirelessLink::cmd_queue_dropped() const
{
    return m_cmd_queue_dropped;
}

// ---- 设置与查询 ----

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

void WirelessLink::notify_link_alive()
{
}

