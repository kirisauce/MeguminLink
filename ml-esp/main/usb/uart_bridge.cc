#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/app.hpp" // Application（CDC 数据按模式分流）
#include "meguminlink/usb/usb.hpp"

#include <driver/uart.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <tinyusb_cdc_acm.h>

static constexpr char TAG[] = "mlink::usb";

namespace
{
// ---- UART bridge 常量（CDC 回调与桥任务共用）----
constexpr auto UART_NUM = static_cast<uart_port_t>(UART_NUM_0 + CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);
constexpr int RX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_RX_BUFFER_SIZE;
constexpr int TX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_TX_BUFFER_SIZE;
constexpr int QUEUE_SIZE = CONFIG_MEGUMINLINK_UART_EVENT_QUEUE_SIZE;

// CDC RX 回调运行在 TinyUSB 任务上下文（tinyusb_task 派发）：只 read + 分流。
// Local 模式：写 UART TX（uart_write_bytes 任务安全，TX 环形缓冲满时阻塞——
// 受 USB 速率上界约束，可接受）；Wireless 模式：打包为 UartData 帧无线转发
// 给配对 peer（尽力而为，未配对丢弃）。
void uart_cdcacm_rx_callback(int interface, [[maybe_unused]] cdcacm_event_t *event)
{
    static u8 rx_buffer[256];
    size_t rx_size = 0;
    const esp_err_t ret =
        tinyusb_cdcacm_read(static_cast<tinyusb_cdcacm_itf_t>(interface), rx_buffer, sizeof(rx_buffer), &rx_size);
    if (ret != ESP_OK || rx_size == 0)
    {
        ESP_LOGW(TAG, "CDC read failed: %s", esp_err_to_name(ret));
        return;
    }

    auto &app = mlink::Application::get_instance();
    if (app.mode() == mlink::Mode::Wireless)
    {
        app.send_uart_data(rx_buffer, rx_size);
        return;
    }

    const int written = uart_write_bytes(UART_NUM, rx_buffer, rx_size);
    if (written < 0)
    {
        ESP_LOGW(TAG, "UART write failed: %s", esp_err_to_name(written));
    }
}

// ---- CDC 行编码回调（主机 SET_LINE_CODING）----
// 运行在 TinyUSB 任务上下文。uart_set_* 为任务安全 API（内部持互斥锁），
// 与 uart_bridge_task 的读写串行化，可直接调用。
// 各参数独立校验：驱动不支持的值（数据位 16、Mark/Space 校验等）告警并跳过该项，
// 其余参数照常应用。
void uart_cdcacm_line_coding_callback(int interface, cdcacm_event_t *event)
{
    if (interface != TINYUSB_CDC_ACM_0 || event->type != CDC_EVENT_LINE_CODING_CHANGED)
    {
        return;
    }
    const cdc_line_coding_t *lc = event->line_coding_changed_data.p_line_coding;
    if (lc == nullptr)
    {
        return;
    }

    auto &app = mlink::Application::get_instance();
    if (app.mode() == mlink::Mode::Wireless)
    {
        // Wireless：线编码无线同步给目标侧 UART（PC 改参数 → C3 实时跟随）。
        // S3 本地 UART1 桥任务已挂起（见 apply_mode），不再配置本地参数。
        app.send_uart_line_coding(lc->bit_rate, lc->data_bits, lc->stop_bits, lc->parity);
        return;
    }

    // 波特率：0 表示"无偏好"（部分主机对虚拟串口发 0），跳过；
    // 驱动对 0 及不可达波特率返回 ESP_FAIL（已核实 LL 层 baud==0 保护），告警后保留旧值
    if (lc->bit_rate != 0)
    {
        const esp_err_t ret = uart_set_baudrate(UART_NUM, lc->bit_rate);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_set_baudrate(%lu) failed: %s", static_cast<unsigned long>(lc->bit_rate),
                     esp_err_to_name(ret));
        }
    }

    // 数据位：5/6/7/8；16 及以上不支持
    switch (lc->data_bits)
    {
    case 5:
        uart_set_word_length(UART_NUM, UART_DATA_5_BITS);
        break;
    case 6:
        uart_set_word_length(UART_NUM, UART_DATA_6_BITS);
        break;
    case 7:
        uart_set_word_length(UART_NUM, UART_DATA_7_BITS);
        break;
    case 8:
        uart_set_word_length(UART_NUM, UART_DATA_8_BITS);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported data bits: %u", lc->data_bits);
        break;
    }

    // 停止位
    switch (lc->stop_bits)
    {
    case CDC_LINE_CODING_STOP_BITS_1:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_1);
        break;
    case CDC_LINE_CODING_STOP_BITS_1_5:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_1_5);
        break;
    case CDC_LINE_CODING_STOP_BITS_2:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_2);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported stop bits: %u", lc->stop_bits);
        break;
    }

    // 校验位：驱动不支持 Mark(3)/Space(4)
    switch (lc->parity)
    {
    case CDC_LINE_CODING_PARITY_NONE:
        uart_set_parity(UART_NUM, UART_PARITY_DISABLE);
        break;
    case CDC_LINE_CODING_PARITY_ODD:
        uart_set_parity(UART_NUM, UART_PARITY_ODD);
        break;
    case CDC_LINE_CODING_PARITY_EVEN:
        uart_set_parity(UART_NUM, UART_PARITY_EVEN);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported parity: %u", lc->parity);
        break;
    }

    // 丢弃按旧参数采样的残余 RX 字节（FIFO + 环形缓冲）。
    // 队列中残留的 UART_DATA 事件无害：桥任务随后读到 0 字节，被既有 len > 0 分支跳过，
    // 无需 xQueueReset。无条件冲刷：参数未变时为空操作，不丢数据。
    uart_flush_input(UART_NUM);

    ESP_LOGI(TAG, "UART%d line coding: %lu baud, %u%c%s", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM,
             static_cast<unsigned long>(lc->bit_rate), lc->data_bits, CDC_LINE_CODING_PARITY_CHAR(lc->parity),
             CDC_LINE_CODING_STOP_BITS_TEXT(lc->stop_bits));
}
} // namespace

namespace mlink
{
void CmsisDapUsb::uart_bridge_task(void *arg)
{
    auto *self = static_cast<CmsisDapUsb *>(arg);
    static u8 buffer[256];
    uart_event_t evt;

    while (true)
    {
        if (xQueueReceive(self->m_uart_queue, &evt, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (evt.type)
        {
        case UART_DATA: {
            const int len = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), 0);
            if (len > 0)
            {
                const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buffer, static_cast<size_t>(len));
                if (queued < static_cast<size_t>(len))
                {
                    ESP_LOGW(TAG, "CDC TX queue full, dropped %d of %d bytes", len - static_cast<int>(queued), len);
                }
                const esp_err_t ret = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
                if (ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "CDC flush failed: %s", esp_err_to_name(ret));
                }
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL: {
            ESP_LOGW(TAG, "UART RX overflow, flushing");
            uart_flush_input(UART_NUM);
            xQueueReset(self->m_uart_queue);
            break;
        }
        default: {
            break; // 其余事件（奇偶校验错误等）忽略
        }
        }
    }
}

esp_err_t CmsisDapUsb::uart_bridge_init()
{
    esp_err_t ret;

    static_assert(RX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));
    static_assert(TX_BUF_SIZE == 0 || TX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));

    ret = uart_driver_install(UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, QUEUE_SIZE, &m_uart_queue, 0);
    MLINK_CHECK("uart_driver_install(UART_NUM_%d, ...) failed", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);

    {
        uart_config_t cfg = {};
        cfg.baud_rate = CONFIG_MEGUMINLINK_UART_BRIDGE_BAUD_RATE;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        ret = uart_param_config(UART_NUM, &cfg);
        MLINK_CHECK("uart_param_config() failed with default configuration");
    }

    ret = uart_set_pin(UART_NUM, CONFIG_MEGUMINLINK_UART_BRIDGE_TX_IO, CONFIG_MEGUMINLINK_UART_BRIDGE_RX_IO, -1, -1);
    MLINK_CHECK("uart_set_pin() failed");

    {
        tinyusb_config_cdcacm_t cfg = {};
        cfg.callback_rx = uart_cdcacm_rx_callback;
        cfg.callback_line_coding_changed = uart_cdcacm_line_coding_callback;
        tinyusb_cdcacm_init(&cfg);
    }

    ret =
        xTaskCreate(uart_bridge_task, "uart_bridge", 4096, this, 5, &m_uart_bridge_task) == pdPASS ? ESP_OK : ESP_FAIL;
    MLINK_CHECK("Failed to create UART bridge task");

    return ESP_OK;

err:
    return ret;
}

void CmsisDapUsb::uart_data_to_cdc(const u8 *data, size_t len)
{
    // 与 uart_bridge_task 的 UART_DATA 分支同构：write_queue 同步拷贝入 CDC
    // TX FIFO，满时返回已入队数；flush 非阻塞发起 IN 传输（短数据必须显式
    // flush 才能送达主机）。尽力而为：FIFO 满丢弃多余字节并告警。
    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    if (queued < len)
    {
        ESP_LOGW(TAG, "CDC TX queue full, dropped %d of %d bytes", static_cast<int>(len - queued),
                 static_cast<int>(len));
    }
    const esp_err_t ret = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "CDC flush failed: %s", esp_err_to_name(ret));
    }
}

void CmsisDapUsb::set_uart_bridge_active(bool active)
{
    // vTaskSuspend/vTaskResume 幂等：重复调用对已处于目标状态的任务无副作用。
    // 任务可能正阻塞在事件队列接收，下个调度点即挂起，无并发风险。
    if (m_uart_bridge_task == nullptr)
    {
        return;
    }
    if (active)
    {
        vTaskResume(m_uart_bridge_task);
    }
    else
    {
        vTaskSuspend(m_uart_bridge_task);
    }
}
} // namespace mlink
