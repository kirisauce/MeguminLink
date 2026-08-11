#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/usb.hpp"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <sdkconfig.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_default_config.h>

#include <cstdio>

static constexpr char TAG[] = "mlink::usb";

namespace
{
// ---- UART bridge 常量（CDC 回调与桥任务共用）----
constexpr auto UART_NUM = static_cast<uart_port_t>(UART_NUM_0 + CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);
constexpr int RX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_RX_BUFFER_SIZE;
constexpr int TX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_TX_BUFFER_SIZE;
constexpr int QUEUE_SIZE = CONFIG_MEGUMINLINK_UART_EVENT_QUEUE_SIZE;

// ---- CMSIS-DAP v2 descriptors ----
// 接口结构：vendor specific 接口 (class 0xFF) + bulk 端点 承载 CMSIS-DAP v2
// 命令/响应，IAD 组合的 CDC ACM 接口作 UART 透传。
// Product String 必须包含 "CMSIS-DAP"，调试器（pyOCD 等）靠它识别设备。

// VID/PID：量产前需注册自有 VID/PID。当前沿用 Espressif TinyUSB 默认值：
// 0x303A 为 Espressif VID，0x4020 为 esp_tinyusb vendor 接口默认 PID (0x4000 | 1<<5)
constexpr u16 USB_VID = 0x303A;
constexpr u16 USB_PID = 0x4020;

// 索引 0 为语言 ID（英文），esp_tinyusb 直接拷贝其原始 2 字节
constexpr char LANGID[] = {0x09, 0x04};

// ---- Device descriptor ----
// bDeviceClass = 0：类在接口级定义（vendor specific）
constexpr tusb_desc_device_t DEVICE_DESCRIPTOR = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// ---- Configuration descriptor ----
// 接口结构：
//   接口 0：vendor specific (0xFF, 0x00, 0x00) —— CMSIS-DAP bulk transport
//     EP2 Bulk OUT（命令）+ EP3 Bulk IN（响应）
//   接口 1/2：IAD 组合的 CDC ACM 串口（UART 透传）
//     EP4 Interrupt IN（通知）+ EP3 Bulk OUT / EP1 Bulk IN（数据）
enum
{
    ITF_NUM_DAP = 0,
    ITF_NUM_CDC, // CDC ACM（通知接口）
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL,

    EP_DAP_OUT = 0x02, // Bulk OUT：命令
    EP_DAP_IN = 0x83,  // Bulk IN ：响应

    EP_CDC_NOTIF = 0x84, // Interrupt IN：CDC 通知
    EP_CDC_OUT = 0x03,   // Bulk OUT：CDC 数据
    EP_CDC_IN = 0x81,    // Bulk IN ：CDC 数据
};

// bmAttributes = 0x80：bus powered
constexpr u8 CONFIG_DESCRIPTOR[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN, 0x80, 500),

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, 4, EP_DAP_OUT, EP_DAP_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EP_CDC_NOTIF, 64, EP_CDC_OUT, EP_CDC_IN, 64),

    // 如需 SWO 流，在此追加 EP3 Bulk In (0x83, 64B) 描述符
};

void tinyusb_event_callback(tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

// CDC RX 回调运行在 TinyUSB 任务上下文（tinyusb_task 派发）：只 read + 写 UART TX。
// uart_write_bytes 是任务安全 API，TX 环形缓冲满时阻塞——受 USB 速率上界约束，可接受。
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
CmsisDapUsb::CmsisDapUsb()
{
    m_string_descriptors[0] = LANGID;
    m_string_descriptors[1] = "MeguminLink";                     // Manufacturer
    m_string_descriptors[2] = "CMSIS-DAP on " CONFIG_IDF_TARGET; // Product（必须含 "CMSIS-DAP"）
    m_string_descriptors[3] = m_serial;                          // Serial（init 时填充）
    m_string_descriptors[4] = "CMSIS-DAP v2";                    // Interface
}

esp_err_t CmsisDapUsb::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;

    {
        // 串号 = 芯片 MAC，保证多个 probe 可同时枚举
        u8 mac[6];
        ret = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
        __check("Failed to read MAC address");
        snprintf(m_serial, sizeof(m_serial), MACSTR, MAC2STR(mac));
    }

    {
        tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG(tinyusb_event_callback);
        tinyusb_config.descriptor.device = &DEVICE_DESCRIPTOR;
        tinyusb_config.descriptor.string = m_string_descriptors;
        tinyusb_config.descriptor.string_count =
            static_cast<int>(sizeof(m_string_descriptors) / sizeof(m_string_descriptors[0]));
        tinyusb_config.descriptor.full_speed_config = CONFIG_DESCRIPTOR;

        ret = tinyusb_driver_install(&tinyusb_config);
        __check("Failed to initialize TinyUSB driver");
    }

    ret = uart_bridge_init();
    __check("Failed to initialize USB CDC ACM interface");

    m_initialized = true;
    return ESP_OK;

err:
    return ret;
}

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
    __check("uart_driver_install(UART_NUM_%d, ...) failed", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);

    {
        uart_config_t cfg = {};
        cfg.baud_rate = CONFIG_MEGUMINLINK_UART_BRIDGE_BAUD_RATE;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        ret = uart_param_config(UART_NUM, &cfg);
        __check("uart_param_config() failed with default configuration");
    }

    ret = uart_set_pin(UART_NUM, CONFIG_MEGUMINLINK_UART_BRIDGE_TX_IO, CONFIG_MEGUMINLINK_UART_BRIDGE_RX_IO, -1, -1);
    __check("uart_set_pin() failed");

    {
        tinyusb_config_cdcacm_t cfg = {};
        cfg.callback_rx = uart_cdcacm_rx_callback;
        cfg.callback_line_coding_changed = uart_cdcacm_line_coding_callback;
        tinyusb_cdcacm_init(&cfg);
    }

    ret = xTaskCreate(uart_bridge_task, "uart_bridge", 4096, this, 5, nullptr) == pdPASS ? ESP_OK : ESP_FAIL;
    __check("Failed to create UART bridge task");

    return ESP_OK;

err:
    return ret;
}
} // namespace mlink
