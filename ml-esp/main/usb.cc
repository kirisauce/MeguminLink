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

bool uart_uhci_handle_rx_event(uhci_controller_t *handle, const uhci_rx_event_data_t *data, void *user_data)
{
    return true;
}

bool uart_uhci_handle_tx_done(uhci_controller_t *handle, const uhci_tx_done_event_data_t *data, void *user_data)
{
    return true;
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

    ret = uart_bridge_init();
    __check("Failed to initialize USB CDC ACM interface");

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

    m_initialized = true;
    return ESP_OK;

err:
    return ret;
}

esp_err_t CmsisDapUsb::uart_bridge_init()
{
    esp_err_t ret;

    constexpr int RX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_RX_BUFFER_SIZE;
    constexpr int TX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_TX_BUFFER_SIZE;
    constexpr auto UART_NUM = static_cast<uart_port_t>(UART_NUM_0 + CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);
    constexpr bool QUEUE_SIZE = CONFIG_MEGUMINLINK_UART_EVENT_QUEUE_SIZE;

    static_assert(RX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));
    static_assert(TX_BUF_SIZE == 0 || TX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));

    ret = uart_driver_install(UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, QUEUE_SIZE, &m_uart_queue, NULL);
    __check("uart_driver_install(UART_NUM_%d, ...) failed", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);

    {
        uart_config_t cfg = {};
        cfg.baud_rate = 115200;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        ret = uart_param_config(UART_NUM, &cfg);
        __check("uart_param_config() failed with default configuration");
    }

    {
        uhci_controller_config_t cfg = {};
        cfg.uart_port = UART_NUM;

        ret = uhci_new_controller(&cfg, &m_uhci);
        __check("uhci_new_controller() failed");
    }

    {
        uhci_event_callbacks_t callbacks = {};
        callbacks.on_rx_trans_event = uart_uhci_handle_rx_event;
        callbacks.on_tx_trans_done = uart_uhci_handle_tx_done;
        ret = uhci_register_event_callbacks(m_uhci, &callbacks, nullptr);
        __check("Failed to register UART UHCI callbacks");
    }

    {
        tinyusb_config_cdcacm_t cfg = {};
        tinyusb_cdcacm_init(&cfg);
    }

    return ESP_OK;

err:
    return ret;
}
} // namespace mlink
