#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless_link.hpp" // WirelessDapExecutor（DapCommand 投递）

#include <driver/gpio.h> // gpio_num_t（m_button.init 参数）
#include <esp_log.h>
#include <esp_mac.h> // MACSTR/MAC2STR（接收端 DAP 命令日志）
#include <sdkconfig.h>

#include <freertos/FreeRTOS.h>

#include <cstring> // memcpy（push_wireless_response）

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
} // namespace

Application::Application()
    : m_mode_key(NVS_KEY_MODE, static_cast<u8>(Mode::Local)), m_mode(Mode::Local),
      m_main_task_wake(xSemaphoreCreateBinary()), m_rx_mutex(xSemaphoreCreateMutex()),
      m_wireless_response_sem(xSemaphoreCreateBinary()), m_wireless_response_mutex(xSemaphoreCreateMutex())
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

void Application::push_wireless_response(const u8 *data, u16 len)
{
    xSemaphoreTake(m_wireless_response_mutex, portMAX_DELAY);
    if (len > sizeof(m_wireless_response))
    {
        // 无线帧承载上限的防御性截断（正常不会触发）
        len = static_cast<u16>(sizeof(m_wireless_response));
    }
    m_wireless_response_len = len;
    std::memcpy(m_wireless_response, data, len);
    xSemaphoreGive(m_wireless_response_mutex);

    // 先写完缓冲再 give：等待方 take 成功后读到的必然是本次完整响应
    xSemaphoreGive(m_wireless_response_sem);
}

bool Application::wait_wireless_response(u8 *buf, u16 *len, u32 timeout_ms)
{
    if (xSemaphoreTake(m_wireless_response_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false; // 超时：调用方丢弃（PC 端 debugger 自行重试）
    }

    xSemaphoreTake(m_wireless_response_mutex, portMAX_DELAY);
    const u16 n = m_wireless_response_len;
    std::memcpy(buf, m_wireless_response, n);
    m_wireless_response_len = 0;
    xSemaphoreGive(m_wireless_response_mutex);

    *len = n;
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
                // 配对控制帧在已配对状态下不应出现，防御性忽略。
                auto packets = take_received_packets();
                for (const auto &rp : packets)
                {
                    switch (rp.packet.packet_type)
                    {
                    case PacketType::DapCommand: {
                        const auto &cmd = rp.packet.payload.dap_command;
                        const esp_err_t ret = m_wireless_link.dap_execute(rp.src_mac, cmd.data, cmd.len);
                        if (ret != ESP_OK)
                        {
                            ESP_LOGW(TAG, "Failed to enqueue DAP command from " MACSTR ": %s", MAC2STR(rp.src_mac),
                                     esp_err_to_name(ret));
                        }
                        break;
                    }
                    case PacketType::DapResponse: {
                        const auto &resp = rp.packet.payload.dap_response;
                        push_wireless_response(resp.data, resp.len);
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
