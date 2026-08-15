#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <driver/gpio.h> // gpio_num_t（m_button.init 参数）
#include <esp_log.h>
#include <esp_mac.h> // MACSTR/MAC2STR（接收端 DAP 命令日志）
#include <sdkconfig.h>

#include <freertos/FreeRTOS.h>

using namespace mlink;

static constexpr char TAG[] = "mlink::app";

namespace
{
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

/**
 * DAP 命令号 → 可读名称（CMSIS-DAP v2 命令 ID，见 DAP.h 的 ID_DAP_*）。
 * 接收端日志用；与 cmsis_dap 实现解耦，不 include DAP.h。
 */
const char *dap_command_name(u8 id)
{
    switch (id)
    {
    case 0x00: return "DAP_Info";
    case 0x01: return "DAP_HostStatus";
    case 0x02: return "DAP_Connect";
    case 0x03: return "DAP_Disconnect";
    case 0x04: return "DAP_TransferConfigure";
    case 0x05: return "DAP_Transfer";
    case 0x06: return "DAP_TransferBlock";
    case 0x07: return "DAP_TransferAbort";
    case 0x08: return "DAP_WriteABORT";
    case 0x09: return "DAP_Delay";
    case 0x0A: return "DAP_ResetTarget";
    case 0x10: return "DAP_SWJ_Pins";
    case 0x11: return "DAP_SWJ_Clock";
    case 0x12: return "DAP_SWJ_Sequence";
    case 0x13: return "DAP_SWD_Configure";
    case 0x14: return "DAP_JTAG_Sequence";
    case 0x15: return "DAP_JTAG_Configure";
    case 0x16: return "DAP_JTAG_IDCODE";
    case 0x17: return "DAP_SWO_Transport";
    case 0x18: return "DAP_SWO_Mode";
    case 0x19: return "DAP_SWO_Baudrate";
    case 0x1A: return "DAP_SWO_Control";
    case 0x1B: return "DAP_SWO_Status";
    case 0x1C: return "DAP_SWO_Data";
    case 0x1D: return "DAP_SWD_Sequence";
    case 0x1E: return "DAP_SWO_ExtendedStatus";
    case 0x1F: return "DAP_UART_Transport";
    case 0x20: return "DAP_UART_Configure";
    case 0x21: return "DAP_UART_Transfer";
    case 0x22: return "DAP_UART_Control";
    case 0x23: return "DAP_UART_Status";
    case 0x7E: return "DAP_QueueCommands";
    case 0x7F: return "DAP_ExecuteCommands";
    default: return "unknown";
    }
}
} // namespace

Application::Application()
    : m_mode_key(NVS_KEY_MODE, static_cast<u8>(Mode::Local)), m_mode(Mode::Local),
      m_main_task_wake(xSemaphoreCreateBinary()), m_rx_mutex(xSemaphoreCreateMutex())
{
    // 延迟构造（get_instance 首次调用，此时 NVS 已初始化）：
    // 从 NVS 恢复模式（Local/Wireless，无记录默认 Local）。
    const u8 v = m_mode_key.get();
    m_mode = (v == static_cast<u8>(Mode::Wireless)) ? Mode::Wireless : Mode::Local;

    // USB 支持未编译（kLocalModeSupported=false）时 Local 不可达：
    // 强制 Wireless，并改写 NVS 自愈（仅发生一次，下次启动直接是 Wireless）。
    if (!kLocalModeSupported && m_mode == Mode::Local)
    {
        m_mode = Mode::Wireless;
        const esp_err_t ret = m_mode_key.set(static_cast<u8>(Mode::Wireless));
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to persist wireless mode (Local unsupported): %s",
                     esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Local mode unsupported (USB disabled); forced wireless");
        }
    }

    // 在此初始化 USB 与模式切换按键，并应用当前模式（USB 常驻，模式差异
    // 在 dap_task 的本地执行/无线转发分流）。
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    ESP_ERROR_CHECK(m_usb.init());
#endif
#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ESP_ERROR_CHECK(m_button.init(static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO)));
#endif
#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
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
    // USB 支持未编译（kLocalModeSupported=false）时 Local 不可达：
    // 静默忽略（不持久化、不应用、无日志），双击切 Local 等价于锁死 Wireless。
    if (new_mode == Mode::Local && !kLocalModeSupported)
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
    // 转发给配对 peer（旧行为：无线模式 tud_disconnect + 挂起 DAP/UART 任务）。
    // 副作用：CDC ACM 串口（UART 桥）在 Wireless 下也保持可用，但不参与转发。
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

#if CONFIG_MEGUMINLINK_USB_SUPPORT
CmsisDapUsb &Application::usb()
{
    return m_usb;
}
#endif

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

void Application::run()
{
    while (true)
    {
        xSemaphoreTake(m_main_task_wake, pdMS_TO_TICKS(200));

#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
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
                if (m_pairing_manual &&
                    xTaskGetTickCount() - m_pairing_manual_start >=
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
                    wireless_link_peer_add(peer_mac);
                }
                // USB 收到的 DAP 命令已由 dap_task 转发给 peer（见 usb.cc）。
                // 这里消费无线链路收到的帧：第一版 DapCommand 仅打日志
                // （不执行、不回响应），配对控制帧在已配对状态下不应出现，
                // 防御性忽略。
                auto packets = take_received_packets();
                for (const auto &rp : packets)
                {
                    if (rp.packet.packet_type == PacketType::DapCommand)
                    {
                        const auto &cmd = rp.packet.payload.dap_command;
                        ESP_LOGI(TAG, "DAP cmd from " MACSTR ": id=0x%02X (%s) len=%u", MAC2STR(rp.src_mac),
                                 cmd.data[0], dap_command_name(cmd.data[0]), static_cast<unsigned>(cmd.len));
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, cmd.data, cmd.len, ESP_LOG_INFO);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Unexpected packet type %d in paired state, ignored",
                                 static_cast<int>(rp.packet.packet_type));
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
