#include "meguminlink/wireless/uart.hpp"
#include "meguminlink/app.hpp" // Application::send_uart_data（UART RX → 无线帧）
#include "meguminlink/common.hpp"

#include <driver/uart.h>
#include <esp_check.h>
#include <esp_log.h>
#include <sdkconfig.h>

static constexpr char TAG[] = "mlink::wireless_uart";

namespace
{
// UART 桥参数与 USB 侧本地桥（usb/uart_bridge.cc）共用同一套 Kconfig
// （见 Kconfig.projbuild，放在 USB 条件之外，非 USB 端也能配置）。
constexpr uart_port_t UART_NUM = static_cast<uart_port_t>(UART_NUM_0 + CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);
constexpr int UART_TX_IO = CONFIG_MEGUMINLINK_UART_BRIDGE_TX_IO;
constexpr int UART_RX_IO = CONFIG_MEGUMINLINK_UART_BRIDGE_RX_IO;
constexpr int UART_BAUD_RATE = CONFIG_MEGUMINLINK_UART_BRIDGE_BAUD_RATE;
constexpr int RX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_RX_BUFFER_SIZE;
constexpr int TX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_TX_BUFFER_SIZE;
constexpr int QUEUE_SIZE = CONFIG_MEGUMINLINK_UART_EVENT_QUEUE_SIZE;

// 单次读取缓冲：≤ 无线帧 payload 上限（MAX_UART_PAYLOAD_SIZE=200）
constexpr size_t READ_CHUNK = 200;
} // namespace

namespace mlink
{
esp_err_t WirelessUartBridge::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;

    static_assert(RX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));
    static_assert(TX_BUF_SIZE == 0 || TX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));

    ret = uart_driver_install(UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, QUEUE_SIZE, &m_uart_queue, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_install(UART_NUM_%d, ...) failed", static_cast<int>(UART_NUM));

    {
        uart_config_t cfg = {};
        cfg.baud_rate = UART_BAUD_RATE;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        ret = uart_param_config(UART_NUM, &cfg);
        ESP_RETURN_ON_ERROR(ret, TAG, "uart_param_config() failed");
    }

    ret = uart_set_pin(UART_NUM, UART_TX_IO, UART_RX_IO, -1, -1);
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_set_pin() failed");

    // 优先级 5（与 S3 本地 UART 桥任务一致）：DAP 执行器（优先级 24）运行
    // 期间本任务被抢占，UART RX 靠硬件 FIFO + 环形缓冲兜底（256B @115200 ≈
    // 22ms 余量，单条 DAP 命令执行为 us~ms 级）。任务平时阻塞在事件队列。
    if (xTaskCreate(bridge_task_fn, "uart-bridge-w", 4096, this, 5, &m_uart_bridge_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create wireless UART bridge task");
        return ESP_ERR_NO_MEM;
    }

    m_initialized = true;
    return ESP_OK;
}

void WirelessUartBridge::write_uart(const u8 *data, size_t len)
{
    const int written = uart_write_bytes(UART_NUM, data, len);
    if (written < 0)
    {
        // TX 环形缓冲满（目标设备未接/背压）：尽力而为，丢弃并告警
        ESP_LOGW(TAG, "UART write failed: %s", esp_err_to_name(written));
    }
}

void WirelessUartBridge::configure(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    // 与 usb/uart_bridge.cc 的 CDC line coding 翻译逻辑对齐。参数为 CDC
    // 枚举原值（目标侧无 tinyusb 头，按数值判断），各参数独立校验：
    // 驱动不支持的值（数据位 16、Mark/Space 校验等）告警并跳过该项。
    if (baud != 0)
    {
        const esp_err_t ret = uart_set_baudrate(UART_NUM, baud);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "uart_set_baudrate(%lu) failed: %s", static_cast<unsigned long>(baud), esp_err_to_name(ret));
        }
    }

    // 数据位：5/6/7/8；16 及以上不支持
    switch (data_bits)
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
        ESP_LOGW(TAG, "Unsupported data bits: %u", data_bits);
        break;
    }

    // 停止位（CDC 枚举）：0=1, 1=1.5, 2=2
    switch (stop_bits)
    {
    case 0:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_1);
        break;
    case 1:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_1_5);
        break;
    case 2:
        uart_set_stop_bits(UART_NUM, UART_STOP_BITS_2);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported stop bits: %u", stop_bits);
        break;
    }

    // 校验位（CDC 枚举）：驱动不支持 Mark(3)/Space(4)
    switch (parity)
    {
    case 0:
        uart_set_parity(UART_NUM, UART_PARITY_DISABLE);
        break;
    case 1:
        uart_set_parity(UART_NUM, UART_PARITY_ODD);
        break;
    case 2:
        uart_set_parity(UART_NUM, UART_PARITY_EVEN);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported parity: %u", parity);
        break;
    }

    // 丢弃按旧参数采样的残余 RX 字节（FIFO + 环形缓冲）
    uart_flush_input(UART_NUM);

    ESP_LOGI(TAG, "UART line coding applied: %lu baud, %u data bits, stop %u, parity %u",
             static_cast<unsigned long>(baud), data_bits, stop_bits, parity);
}

void WirelessUartBridge::bridge_task_fn(void *arg)
{
    auto *self = static_cast<WirelessUartBridge *>(arg);
    static u8 buffer[READ_CHUNK];
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
            // 循环读尽：UART_DATA 事件只在数据到达时触发一次，若一次读不完
            // （缓冲 > READ_CHUNK），剩余字节不会再有事件，须在此读完。
            while (true)
            {
                const int len = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), 0);
                if (len <= 0)
                {
                    break;
                }
                Application::get_instance().send_uart_data(buffer, static_cast<size_t>(len));
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
} // namespace mlink
