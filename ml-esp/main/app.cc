#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/usb/usb_capability.hpp"
#include "meguminlink/wireless/arq_link.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/link.hpp" // WirelessDapExecutor（DapCommand 投递）

#include <driver/gpio.h> // gpio_num_t（m_button.init 参数）
#include <esp_log.h>
#include <esp_mac.h> // MACSTR/MAC2STR（接收端 DAP 命令日志）
#include <esp_system.h>
#include <sdkconfig.h>

#include <freertos/FreeRTOS.h>

#include <cstring> // memcpy（push_wireless_response）

using namespace mlink;

static constexpr char TAG[] = "mlink::app";

namespace
{
/**
 * Reset/Reboot 重启前的等待。响应帧已交给 EP0（DATA IN 阶段），但主循环
 * 可能先于传输完成执行到这里；直接重启会让主机看到传输中断。留出一个
 * USB 帧级的余量（全速帧 1ms）确保响应送达。
 */
constexpr u32 RESTART_DELAY_MS = 100;

const char *mode_to_string(Mode mode)
{
    switch (mode)
    {
    case Mode::Local:
        return "local";
    case Mode::Wireless:
        return "wireless";
    }
    return "?";
}
} // namespace

Application::Application()
    : m_mode_key(NVS_KEY_MODE, static_cast<u8>(Mode::Local)), m_mode(Mode::Local),
      m_main_task_wake(xSemaphoreCreateBinary()), m_rx_mutex(xSemaphoreCreateMutex()),
      m_wireless_response_queue(xQueueCreate(CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH, sizeof(WirelessResponse)))
{
    // 延迟构造（get_instance 首次调用，此时 NVS 已初始化）：
    // 从 NVS 恢复模式（Local/Wireless，无记录默认 Local）。
    const u8 v = m_mode_key.get();
    m_mode = (v == static_cast<u8>(Mode::Wireless)) ? Mode::Wireless : Mode::Local;

    // USB 支持未编译（LOCAL_MODE_SUPPORTED=false）时 Local 不可达：
    // 强制 Wireless，并改写 NVS 自愈（仅发生一次，下次启动直接是 Wireless）。
    if (!LOCAL_MODE_SUPPORTED && m_mode == Mode::Local)
    {
        m_mode = Mode::Wireless;
        const esp_err_t ret = m_mode_key.set(static_cast<u8>(Mode::Wireless));
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to persist wireless mode (Local unsupported): %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Local mode unsupported (USB disabled); forced wireless");
        }
    }

    // 在此初始化 USB 与模式切换按键，并应用当前模式（USB 常驻，模式差异
    // 在 dap_task 的本地执行/无线转发分流）。
    // 无线链路初始化（WiFi/ESP-NOW/广播 peer + 无线侧 DAP 执行器）：
    // 任何构建都创建，仅主循环投递命令时工作（Local 模式下空闲阻塞）
    ESP_ERROR_CHECK(m_wireless_link.init());
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    // USB 先起：本机串口桥的对侧端口（CDC）由它提供
    ESP_ERROR_CHECK(m_usb.init());
#endif
    // 本机串口桥：驱动、两个队列与两条任务都由它自己建立，两端同一实现
    ESP_ERROR_CHECK(m_uart_bridge.init());
#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ESP_ERROR_CHECK(m_button.init(static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO)));
#endif
#if !CONFIG_MEGUMINLINK_LED_DRIVER_NONE
    ESP_ERROR_CHECK(m_leds.init(*this));
#endif
    apply_mode();
}

Mode Application::mode() const
{
    return m_mode;
}

void Application::set_mode(Mode new_mode)
{
    // USB 支持未编译（LOCAL_MODE_SUPPORTED=false）时 Local 不可达：
    // 静默忽略（不持久化、不应用、无日志），双击切 Local 等价于锁死 Wireless。
    if (new_mode == Mode::Local && !LOCAL_MODE_SUPPORTED)
    {
        return;
    }

    m_mode = new_mode; // 先更新状态：apply_mode 按新模式应用系统行为

    // 配对是 Wireless 子流程：离开 Wireless 即终止配对（切回时未配对会重新自动进入）
    if (new_mode != Mode::Wireless && m_pairing_active)
    {
        leave_pairing("mode switch");
    }

    // 持久化到 NVS（带缓存：相同值自动跳过落盘）
    const esp_err_t ret = m_mode_key.set(static_cast<u8>(new_mode));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to store mode: %s", esp_err_to_name(ret));
    }
    apply_mode();
    xSemaphoreGive(m_main_task_wake);
}

void Application::apply_mode()
{
    // USB 设备常驻启用（Local 与 Wireless 均保持）：Wireless 模式下不再
    // 整体禁用 USB，而是由 usb.cc 的 dap_task 把 USB 收到的 DAP 命令无线
    // 转发给配对 peer。
    // UART 桥任务同样常驻，两个模式都由回调分流：Local 把 CDC 与本地 UART1
    // 对接，Wireless 把本机端口的数据转发给配对 peer、把对端来的数据写回本机
    // 对侧端口。
    ESP_LOGI(TAG, "Mode: %s", mode_to_string(m_mode));
}

SemaphoreHandle_t Application::main_task_wake()
{
    return m_main_task_wake;
}

void Application::push_received_packet(const ReceivedPacket &pkt)
{
    xSemaphoreTake(m_rx_mutex, portMAX_DELAY);
    m_received_packets.push_back(pkt);
    xSemaphoreGive(m_rx_mutex);

    // ESP-NOW 接收回调运行在 esp_wifi 任务上下文（非 ISR），用普通 Give
    xSemaphoreGive(m_main_task_wake);
}

std::vector<ReceivedPacket> Application::take_received_packets()
{
    std::vector<ReceivedPacket> packets;
    xSemaphoreTake(m_rx_mutex, portMAX_DELAY);
    packets.swap(m_received_packets);
    xSemaphoreGive(m_rx_mutex);
    return packets;
}

void Application::push_wireless_response(u16 seq, const u8 *data, u16 len)
{
    if (m_wireless_response_queue == nullptr)
    {
        return;
    }

    WirelessResponse response = {};
    if (len > sizeof(response.data))
    {
        // 无线帧承载上限的防御性截断（正常不会触发）
        len = static_cast<u16>(sizeof(response.data));
    }
    response.seq = seq;
    response.len = len;
    std::memcpy(response.data, data, len);

    // 队列满说明消费侧没跟上：丢弃并告警，等这条响应的命令最终由超时销账
    if (xQueueSend(m_wireless_response_queue, &response, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "wireless response queue full, dropped seq=%u (%u bytes)", seq, len);
    }
}

bool Application::wait_wireless_response(u16 *seq, u8 *buf, u16 *len, u32 timeout_ms)
{
    if (m_wireless_response_queue == nullptr)
    {
        return false;
    }

    WirelessResponse response = {};
    if (xQueueReceive(m_wireless_response_queue, &response, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false; // 超时或队列为空：调用方按自己的节奏再来取
    }

    if (seq != nullptr)
    {
        *seq = response.seq;
    }
    std::memcpy(buf, response.data, response.len);
    *len = response.len;
    return true;
}

#if CONFIG_MEGUMINLINK_USB_SUPPORT
CmsisDapUsb &Application::usb()
{
    return m_usb;
}
#endif

WirelessLink &Application::wireless_link()
{
    return m_wireless_link;
}

UartBridge &Application::uart_bridge()
{
    return m_uart_bridge;
}

ConfigChannel &Application::config_channel()
{
    return m_config_channel;
}

void Application::forward_uart_data_from_port(const u8 *data, size_t len, UartSource source)
{
    // 本机某个端口收到数据，按当前模式选路。路径实现各自在自己的对象里：
    // 无线在 WirelessLink、本机 UART 在 UartBridge、CDC 在 CmsisDapUsb。
    if (m_mode == Mode::Wireless)
    {
        const esp_err_t ret = m_wireless_link.send_uart_data(data, len, source);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Wireless UART: failed to send %u bytes: %s", static_cast<unsigned>(len), esp_err_to_name(ret));
        }
        return;
    }

#if CONFIG_MEGUMINLINK_USB_SUPPORT && CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    // Local：CDC 与本机 UART 直通，不经过无线。数据从哪来就写到对面去。
    if (source == UartSource::Host)
    {
        m_uart_bridge.send_uart_data(data, len);
    }
    else
    {
        m_config_channel.add_uart_tx(static_cast<u16>(m_usb.cdc().send_uart_data(data, len)));
    }
#else
    // 直通的另一端不存在：无 USB 的一端（LOCAL_MODE_SUPPORTED 为假，上电即
    // Wireless，走不到这里），或 CDC 接口未编译。丢弃。
    (void)data;
    (void)len;
    (void)source;
#endif
}

void Application::forward_uart_data_from_wireless(const u8 *data, size_t len, UartSource source)
{
    // 无线来的帧：写本机端口。recv_cb 跑在 esp_wifi 任务上下文，而写 UART 会
    // 阻塞，所以桥只做一次拷贝入队，真正的写入在它自己的 tx 任务里。
    m_uart_bridge.push_inbound(data, len, source);
}

void Application::route_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    // 与 forward_uart_data_from_port 同一套选路：Wireless 同步给对端，Local 直接设本机 UART。
    if (m_mode == Mode::Wireless)
    {
        const esp_err_t ret = m_wireless_link.send_line_coding(baud, data_bits, stop_bits, parity);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Wireless UART: failed to send line coding: %s", esp_err_to_name(ret));
        }
        return;
    }

    m_uart_bridge.configure(baud, data_bits, stop_bits, parity);
}

void Application::enter_pairing(bool manual)
{
    if (m_pairing_active)
    {
        return; // 已在配对流程（含自动配对中）：长按/重复进入幂等忽略
    }
    m_pairing_active = true;
    m_pairing_manual = manual;
    m_pairing.reset();
    // 配对期间长按识别一律抑制：任意按下即退出的语义只由手动配对消费，
    // 自动配对中的长按因此不会叠加手动配对
    m_button.set_long_press_suppressed(true);
    if (manual)
    {
        m_pairing_manual_start = xTaskGetTickCount();
        ESP_LOGI(TAG, "Entering pairing (manual; NVS pairing kept, timeout %d ms)",
                 CONFIG_MEGUMINLINK_PAIRING_MODE_TIMEOUT_MS);
    }
    else
    {
        ESP_LOGI(TAG, "Entering pairing (auto: no peer in NVS)");
    }
}

void Application::leave_pairing(const char *reason)
{
    if (!m_pairing_active)
    {
        return;
    }
    m_pairing_active = false;
    m_pairing_manual = false;
    m_button.set_long_press_suppressed(false);
    m_pairing.reset();
    ESP_LOGI(TAG, "Left pairing (%s)", reason);
}

bool Application::pairing_active() const
{
    return m_pairing_active;
}

void Application::pairing_completed()
{
    // 配对成功落盘后由 PairingState 回调：统一退出配对流程
    leave_pairing("completed");
}

void Application::clear_pairing()
{
    // 先取旧记录（擦除后就取不到了）：NVS 记录与 ESP-NOW peer 表一起清，
    // 后者在重启后本来就是空的，但运行期残留会让旧 peer 仍可发帧过来。
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (peer_mac_load(peer_mac))
    {
        const esp_err_t ret = m_wireless_link.peer_remove(peer_mac);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to remove peer " MACSTR ": %s", MAC2STR(peer_mac), esp_err_to_name(ret));
        }
    }
    (void)peer_mac_erase();

    // 无条件重开一轮配对：配对流程进行中时先退出，让状态机从头开始
    if (m_pairing_active)
    {
        leave_pairing("clear pairing");
    }
    if (m_mode == Mode::Wireless)
    {
        enter_pairing(false); // 无旧记录可恢复：以配对成功为唯一出口
    }
}

void Application::consume_pending_action()
{
    ConfigChannel::Action action = ConfigChannel::Action::None;
    u8 arg = 0;
    if (!m_config_channel.take_action(&action, &arg))
    {
        return;
    }

    switch (action)
    {
    case ConfigChannel::Action::None:
        break;
    case ConfigChannel::Action::SetMode:
        set_mode(arg == static_cast<u8>(Mode::Wireless) ? Mode::Wireless : Mode::Local);
        break;
    case ConfigChannel::Action::EnterPairing:
        if (m_mode == Mode::Wireless)
        {
            enter_pairing(false);
        }
        else
        {
            ESP_LOGI(TAG, "EnterPairing ignored in local mode; switch to wireless first");
        }
        break;
    case ConfigChannel::Action::ExitPairing:
        if (m_mode == Mode::Wireless && m_pairing_active)
        {
            leave_pairing("config channel");
        }
        else
        {
            ESP_LOGI(TAG, "ExitPairing ignored: not pairing in wireless mode");
        }
        break;
    case ConfigChannel::Action::Ping:
        // 上位机下发的立即探测（WebUI 定时/点击、CLI ping）。Local 模式没有可用
        // 的无线对端，忽略并记日志（与 EnterPairing 同语义）
        if (m_mode == Mode::Wireless)
        {
            m_config_channel.start_ping();
        }
        else
        {
            ESP_LOGI(TAG, "Ping ignored in local mode; switch to wireless first");
        }
        break;
    case ConfigChannel::Action::ClearPair:
        clear_pairing();
        break;
    case ConfigChannel::Action::Reset:
        // 删除失败不回滚：重启后的构造逻辑会按残留记录自愈
        ESP_LOGI(TAG, "Factory reset: erasing pairing, mode and WiFi rate, rebooting");
        (void)peer_mac_erase();
        (void)m_mode_key.erase();
#if CONFIG_MEGUMINLINK_USB_SUPPORT
        (void)usb_capability_erase();
#endif
        // 速率档位不做持久化，这里显式回默认档：恢复出厂设置后射频参数也应回到默认
        (void)m_wireless_link.set_wifi_rate(DEFAULT_WIFI_RATE);
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
        // 注入丢包率同理：恢复出厂设置必须回到不注入
        (void)m_wireless_link.set_loss_permille(DEFAULT_LOSS_PERMILLE);
#endif
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
        break;
    case ConfigChannel::Action::Reboot:
        ESP_LOGI(TAG, "Reboot requested");
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
        break;
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    case ConfigChannel::Action::ApplyUsbConfig: {
        UsbCapabilitySettings draft;
        if (!m_config_channel.take_usb_config_draft(&draft))
        {
            break;
        }

        // 与生效值一致时不重新枚举：省掉一次无意义的断连（落盘也由 NVS 键的
        // 相同值跳过落盘兜住）
        if (draft == m_usb.usb_capability())
        {
            ESP_LOGI(TAG, "USB capabilities unchanged; re-enumeration skipped");
            break;
        }

        const esp_err_t ret = usb_capability_store(draft);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to store USB capabilities: %s", esp_err_to_name(ret));
        }
        m_usb.apply_usb_capability(draft);
        break;
    }
#else
    case ConfigChannel::Action::ApplyUsbConfig:
        // 无 USB 侧不存在 USB 能力（既没有可重建的描述符，也没有草稿槽）：
        // 只有对端转发来的控制请求可能投递到这个动作，记日志忽略
        ESP_LOGW(TAG, "ApplyUsbConfig ignored: no USB on this target");
        break;
#endif
    case ConfigChannel::Action::SetWifiRate:
        // ESP-NOW 发送速率档位。应用会操作 WiFi 驱动，因此只能在这里（主循环）
        // 做——命令的接受方既可能是本机 EP0，也可能是转发的对端请求。
        (void)m_wireless_link.set_wifi_rate(arg);
        break;
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    case ConfigChannel::Action::SetLinkConfig: {
        // 参数落盘 + 立即应用。应用会换新 epoch 重置会话，对端据此重新对齐序号并
        // 采纳这份参数——参数因此不必再走那条尽力而为的中继通道。
        ArqSettings settings = {};
        if (m_config_channel.take_link_config_draft(&settings) &&
            m_wireless_link.arq_apply_settings(settings, static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS)) ==
                ESP_OK)
        {
            const esp_err_t ret = arq_settings_store(settings);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to persist ARQ settings: %s", esp_err_to_name(ret));
            }
            ESP_LOGI(TAG, "Reliable transport settings applied (generation %u, window %u, rto %u-%u ms, retx %u)",
                     static_cast<unsigned>(settings.generation), settings.window, settings.rto_min_ms,
                     settings.rto_max_ms, settings.max_retx);
        }
        break;
    }
#endif

    case ConfigChannel::Action::SetLoss:
        // 注入丢包率（千分比）。只在发送端生效，两端各自由这条命令设置自己的值。
        (void)m_wireless_link.set_loss_permille(arg);
        break;
#endif
    }
}

void Application::run()
{
    while (true)
    {
        // 空闲等待按可靠传输的下一个待办时刻收短：纯确认、重传与握手帧都靠主循环
        // 在空闲时被驱动，睡满 200ms 会让它们迟到（对端会因此白重传一轮）
        u32 idle_ms = 200;
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
        {
            const u32 arq_deadline = m_wireless_link.arq_next_deadline_ms();
            const u32 now = static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (arq_deadline != 0)
            {
                const u32 until = static_cast<u32>(static_cast<i32>(arq_deadline - now));
                if (until < idle_ms)
                {
                    idle_ms = (until == 0) ? 1 : until;
                }
            }
        }
#endif
        xSemaphoreTake(m_main_task_wake, pdMS_TO_TICKS(idle_ms));

        // 配置通道投递的动作先执行：模式与配对状态随之改变，LED 随后刷新
        consume_pending_action();

        // 无线侧与对端的档位同步：接收回调只登记待生效值（不可在回调里操作
        // WiFi 驱动），实际应用在这里
        m_wireless_link.poll();

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
        // 可靠传输的空闲驱动：重传、纯确认、响应重放与会话握手帧都在这里发出
        m_wireless_link.arq_poll(static_cast<u32>(xTaskGetTickCount() * portTICK_PERIOD_MS));

        // 参数被改动过（主机下发，或按代数/MAC 采纳了对端那份）：落盘。NVS 写入
        // 只在主循环做，不占 tinyusb 任务，也不占 WiFi 任务（接收回调会改参数）。
        if (m_wireless_link.arq_take_settings_dirty())
        {
            const esp_err_t persist_ret = arq_settings_store(m_wireless_link.arq_settings());
            if (persist_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to persist ARQ settings: %s", esp_err_to_name(persist_ret));
            }
        }
#endif

        // 对端存活探测只做超时结算：发起的时机由上位机掌握（配置通道 0x18）
        m_config_channel.poll_ping_timeout();

#if !CONFIG_MEGUMINLINK_LED_DRIVER_NONE
        // LED 随主循环节拍刷新：模式/配对变化（含按键唤醒）最迟一个节拍内反映
        m_leds.update();
#endif

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
        // 按键事件由 m_button（状态机）识别，这里只消费并执行动作：
        // 1. 双击 → 切换模式（本地 <-> 无线）
        if (m_button.consume_double_click())
        {
            set_mode(mode() == Mode::Wireless ? Mode::Local : Mode::Wireless);
        }

        // 2. 任意一次按下 → 手动进入的配对流程立即退出（自动配对不响应按键）
        if (m_button.consume_button_press() && m_pairing_active && m_pairing_manual)
        {
            leave_pairing("button press");
        }

        // 3. 长按 → 手动进入配对流程（仅 Wireless 有效，Local 忽略并提示）；
        //    自动配对中长按被 enter_pairing 幂等忽略
        if (m_button.consume_long_press())
        {
            if (mode() == Mode::Wireless)
            {
                enter_pairing(true);
            }
            else
            {
                ESP_LOGI(TAG, "Long press ignored in local mode; switch to wireless first");
            }
        }
#endif

        switch (mode())
        {
        case Mode::Local: {
            // 丢弃积压的配对帧（本地调试不处理无线协议）
            take_received_packets();
            break;
        }
        case Mode::Wireless: {
            // 统一配对入口：未配对 → 自动进入配对流程；已配对 → 无线调试
            if (!m_pairing_active)
            {
                u8 peer_mac[ESP_NOW_ETH_ALEN];
                if (!peer_mac_load(peer_mac))
                {
                    enter_pairing(false);
                }
            }

            if (m_pairing_active)
            {
                // 统一驱动配对状态机。手动进入的配对有超时退出（防误按恢复旧配对）；
                // 自动进入的配对以配对成功为唯一出口（超时退出无旧配对可用，无意义）
                if (m_pairing_manual && xTaskGetTickCount() - m_pairing_manual_start >=
                                            pdMS_TO_TICKS(CONFIG_MEGUMINLINK_PAIRING_MODE_TIMEOUT_MS))
                {
                    leave_pairing("timeout");
                }
                else
                {
                    m_pairing.process(*this);
                }
            }
            else
            {
                // 已配对：无线调试就绪。先确保 ESP-NOW peer 已登记——配对完成
                // 时 drop_peer() 会移除 peer（pairing.cc），重启后 peer list 也为空，
                // 而 dap_task 转发依赖 peer 在 ESP-NOW list 中；peer_add 幂等，
                // 周期调用开销可忽略，配对完成后下一轮循环即恢复转发。
                u8 peer_mac[ESP_NOW_ETH_ALEN];
                if (peer_mac_load(peer_mac))
                {
                    m_wireless_link.peer_add(peer_mac);
                }
                // USB 收到的 DAP 命令已由 dap_task 转发给 peer（见 usb.cc）。
                // 这里消费无线链路收到的帧：
                // - DapCommand：本机是接收端（target 侧），投递给 wireless_dap
                //   执行器（DAP_ExecuteCommand + 响应回传，见 wireless_dap.cc）；
                // - DapResponse：本机是发送端，存入响应通道唤醒 dap_task
                //   （dap_task 转发命令后正等待该响应并写回 USB）；
                // - Ping：对端在探测本机，原样回显 token 回一帧 Pong；
                // - Pong：对端回显本机探测，交给配置通道结算往返时延；
                // 配对控制帧在已配对状态下不应出现，防御性忽略。
                auto packets = take_received_packets();
                for (const auto &rp : packets)
                {
                    switch (rp.packet.packet_type)
                    {
                    case PacketType::DapCommand: {
                        const auto &cmd = rp.packet.payload.dap_command;
                        // 装了可靠传输时 DAP 命令在接收回调里就地消费（重复帧要立即
                        // 重放），不会走到这条经主循环的老路径；序号填 0 只为兼容。
                        // 通道标记照常透传：执行器靠它回答 DAP_Info 的单包尺寸
                        const esp_err_t ret =
                            m_wireless_link.dap_execute(rp.src_mac, 0, cmd.data, cmd.len, cmd.channel);
                        if (ret != ESP_OK)
                        {
                            ESP_LOGW(TAG, "Failed to enqueue DAP command from " MACSTR ": %s", MAC2STR(rp.src_mac),
                                     esp_err_to_name(ret));
                        }
                        break;
                    }
                    case PacketType::DapResponse: {
                        const auto &resp = rp.packet.payload.dap_response;
                        // 未经可靠传输的响应没有序号：填 0，配对退化为先进先出
                        push_wireless_response(0, resp.data, resp.len);
                        break;
                    }
                    case PacketType::Ping: {
                        // 回包在主循环任务上下文发出：ESP-NOW 接收回调只投递，
                        // 不在回调里调用发送（见 config_channel.hpp 的探测说明）
                        const esp_err_t ret = m_wireless_link.send_pong(rp.src_mac, rp.packet.payload.ping.token);
                        if (ret != ESP_OK)
                        {
                            ESP_LOGW(TAG, "Failed to reply pong to " MACSTR ": %s", MAC2STR(rp.src_mac),
                                     esp_err_to_name(ret));
                        }
                        break;
                    }
                    case PacketType::Pong: {
                        // 任何 Pong 都说明链路双向可用：熔断中的发送侧据此自愈
                        // （熔断期的探针 Ping 也走这里，token 由配置通道自行判弃）
                        m_wireless_link.notify_link_alive();
                        m_config_channel.on_pong(rp.src_mac, rp.packet.payload.pong.token);
                        break;
                    }
                    default:
                        ESP_LOGW(TAG, "Unexpected packet type %d in paired state, ignored",
                                 static_cast<int>(rp.packet.packet_type));
                        break;
                    }
                }
            }
            break;
        }
        }
    }
}

Application &Application::get_instance()
{
    // 延迟构造：首次调用（app_main 中 NVS 初始化后）才构造实例，
    // 保证构造函数里的 NVS 读取与 USB 初始化可用。
    static Application instance;
    return instance;
}
