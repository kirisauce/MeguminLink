#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/wireless_uart.hpp"
#include "esp_log.h"
#include "meguminlink/app.hpp" // Application::send_uart_data（UART RX → 无线帧）
#include "meguminlink/common.hpp"

#include <driver/uart.h>
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
    MLINK_CHECK("uart_driver_install(UART_NUM_%d, ...) failed", static_cast<int>(UART_NUM));

    {
        uart_config_t cfg = {};
        cfg.baud_rate = UART_BAUD_RATE;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        ret = uart_param_config(UART_NUM, &cfg);
        MLINK_CHECK("uart_param_config() failed");
    }

    ret = uart_set_pin(UART_NUM, UART_TX_IO, UART_RX_IO, -1, -1);
    MLINK_CHECK("uart_set_pin() failed");

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

err:
    return ret;
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
