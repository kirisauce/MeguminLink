#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
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

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
// 防抖窗口：忽略间隔小于该值的边沿抖动（机械抖动通常 <5ms，20ms 足够）。
// 必须远小于双击窗口（默认 400ms），否则真实双击会被防抖吞掉。
constexpr u64 DEBOUNCE_US = 20 * 1000ULL;

// 以下变量仅 ISR 写（s_double_click_pending 由主任务消费并清除）。
// bool 是单字节对齐访问，在 32 位目标上读写原子，volatile 防止编译器缓存。
volatile u64 s_last_edge_us = 0;   // 最近一次有效（防抖后）边沿时间
volatile u64 s_first_press_us = 0; // 双击窗口内的第一次按下时间；0 = 窗口空闲
volatile bool s_double_click_pending = false;

void IRAM_ATTR mode_switch_isr(void *)
{
    const u64 now = esp_timer_get_time();

    // 防抖：抖动产生的密集边沿直接忽略
    if (now - s_last_edge_us < DEBOUNCE_US)
    {
        return;
    }
    s_last_edge_us = now;

    const u64 first = s_first_press_us;
    if (first != 0 && now - first <= CONFIG_MEGUMINLINK_MODE_SWITCH_DOUBLE_CLICK_MS * 1000ULL)
    {
        // 窗口内的第二次按下 → 双击；复位 first_press，避免三击连切
        s_double_click_pending = true;
        s_first_press_us = 0;
    }
    else
    {
        // 第一次按下，或上一次窗口已过期（自然成为新的第一次按下）
        s_first_press_us = now;
    }
}

bool consume_double_click()
{
    if (s_double_click_pending)
    {
        s_double_click_pending = false;
        return true;
    }
    return false;
}

esp_err_t mode_switch_button_init()
{
    const gpio_num_t gpio = static_cast<gpio_num_t>(CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO);

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE; // 默认 GPIO0（BOOT 键）：内部上拉，按下拉低
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_NEGEDGE; // 按下 → 下降沿

    esp_err_t ret = gpio_config(&cfg);
    __check("gpio_config(GPIO%d) failed", CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO);

    // gpio_install_isr_service 全局唯一，重复调用返回 ESP_ERR_INVALID_STATE；
    // 本模块是项目中第一个使用 GPIO ISR 服务的组件（调用前已确认）。
    ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    __check("gpio_install_isr_service failed");

    ret = gpio_isr_handler_add(gpio, mode_switch_isr, nullptr);
    __check("gpio_isr_handler_add(GPIO%d) failed", CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO);

    ESP_LOGI(TAG, "Mode switch on GPIO%d (double click, %d ms window)", CONFIG_MEGUMINLINK_MODE_SWITCH_GPIO,
             CONFIG_MEGUMINLINK_MODE_SWITCH_DOUBLE_CLICK_MS);
    return ESP_OK;

err:
    return ret;
}
#endif
} // namespace

Application::Application()
    : m_mode_key(NVS_KEY_MODE, static_cast<u8>(Mode::Local)), m_mode(Mode::Local),
      m_main_task_wake(xSemaphoreCreateBinary()), m_rx_mutex(xSemaphoreCreateMutex())
{
    // 延迟构造（get_instance 首次调用，此时 NVS 已初始化）：
    // 从 NVS 恢复模式（Local/Wireless，无记录默认 Local）。
    const u8 v = m_mode_key.get();
    m_mode = (v == static_cast<u8>(Mode::Wireless)) ? Mode::Wireless : Mode::Local;

    // 在此初始化 USB 与模式切换按键，并应用当前模式（无线模式立即禁用 USB）。
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    ESP_ERROR_CHECK(m_usb.init());
#endif
#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ESP_ERROR_CHECK(mode_switch_button_init());
#endif
    apply_mode();
}

Mode Application::mode() const
{
    return m_mode;
}

void Application::set_mode(Mode new_mode)
{
    m_mode = new_mode; // 先更新状态：apply_mode 按新模式应用系统行为

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
    // 无线模式禁用整个 USB 设备（主机端表现为拔线），本地模式恢复
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    m_usb.set_enabled(m_mode == Mode::Local);
#endif
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

void Application::run()
{
    while (true)
    {
        xSemaphoreTake(m_main_task_wake, pdMS_TO_TICKS(200));

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
        // 双击按键切换模式（本地 <-> 无线）。ISR 置位、此处消费；
        // 200ms 轮询粒度对双击切换的响应无感。
        if (consume_double_click())
        {
            set_mode(mode() == Mode::Wireless ? Mode::Local : Mode::Wireless);
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
            // 无线调试：已配对（NVS 有 peer 记录）→ 占位逻辑（TODO 转发调试协议）；
            // 未配对 → 自动进入配对子流程
            u8 peer_mac[ESP_NOW_ETH_ALEN];
            if (peer_mac_load(peer_mac))
            {
                // TODO(无线调试)：在此将调试数据转发到 peer（复用 ESP-NOW 链路）
                take_received_packets();
            }
            else
            {
                m_pairing.process(*this);
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
