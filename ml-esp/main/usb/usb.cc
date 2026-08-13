#define __MEGUMINLINK_EXPORT_MACROS__
#include "meguminlink/usb/usb.hpp"
#include "meguminlink/app.hpp"

#include <esp_log.h>
#include <esp_mac.h>
#include <sdkconfig.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tusb.h>

#include <cstdio>
#include <cstring>

#include "DAP_config.h"
#include "DAP.h"

static constexpr char TAG[] = "mlink::usb";

namespace
{
// ---- CMSIS-DAP v2 descriptors ----
// 接口结构：vendor specific 接口 (class 0xFF) + bulk 端点 承载 CMSIS-DAP v2
// 命令/响应；CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加 IAD 组合的
// CDC ACM 接口作 UART 透传。
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
//   CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加：
//   接口 1/2：IAD 组合的 CDC ACM 串口（UART 透传）
//     EP4 Interrupt IN（通知）+ EP3 Bulk OUT / EP1 Bulk IN（数据）
enum
{
    ITF_NUM_DAP = 0,
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    ITF_NUM_CDC, // CDC ACM（通知接口）
    ITF_NUM_CDC_DATA,
#endif
    ITF_NUM_TOTAL,

    EP_DAP_OUT = 0x02, // Bulk OUT：命令
    EP_DAP_IN = 0x83,  // Bulk IN ：响应

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    EP_CDC_NOTIF = 0x84, // Interrupt IN：CDC 通知
    EP_CDC_OUT = 0x03,   // Bulk OUT：CDC 数据
    EP_CDC_IN = 0x81,    // Bulk IN ：CDC 数据
#endif
};

// bmAttributes = 0x80：bus powered
constexpr u8 CONFIG_DESCRIPTOR[] = {
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN, 0x80, 500),
#else
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0x80, 500),
#endif

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, 4, EP_DAP_OUT, EP_DAP_IN, 64),
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EP_CDC_NOTIF, 64, EP_CDC_OUT, EP_CDC_IN, 64),
#endif

    // 如需 SWO 流，在此追加 EP3 Bulk In (0x83, 64B) 描述符
};

void tinyusb_event_callback(tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

// ---- CMSIS-DAP v2 协议处理 ----
// 请求经 RX ring 进入 DAP_ExecuteCommand，响应直写 vendor TX FIFO。
// 上游 main.c 中 USB 驱动调用 DAP_ExecuteCommand 的语义等同于此任务循环。

// CMSIS-DAP v2 bulk：单次 USB 传输 = 一个请求包（EP 64B ⇒ 每项 ≤64B）
u8 dap_request[DAP_PACKET_SIZE];
u8 dap_response[DAP_PACKET_SIZE];

// 响应写入 vendor TX FIFO：write 同步拷贝入 FIFO，满时返回 0；
// tinyusb_task 后台排空。拷贝完成即缓冲区可复用，无需等发送完毕。
void dap_send_response(u16 len)
{
    u16 sent = 0;
    while (sent < len)
    {
        if (!tud_vendor_n_mounted(ITF_NUM_DAP))
        {
            tud_vendor_n_write_clear(ITF_NUM_DAP); // 断开：丢弃未发数据
            return;
        }
        const u32 n = tud_vendor_n_write(ITF_NUM_DAP, dap_response + sent, len - sent);
        if (n > 0)
        {
            sent += static_cast<u16>(n);
        }
        else
        {
            vTaskDelay(1); // FIFO 满，退避等排空
        }
    }
    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：写入字节数 < EP size 时
    // 不会自动发起 IN 传输，短响应必须显式 flush 才能送达主机
    tud_vendor_n_write_flush(ITF_NUM_DAP);
}

void dap_task(void *arg)
{
    auto &usb = *reinterpret_cast<mlink::CmsisDapUsb *>(arg);
    size_t item_size;

    DAP_Setup(); // 与上游 main.c 一致：初始化 DAP 状态与 SWJ 时钟

    while (true)
    {
        auto item = reinterpret_cast<u8 *>(xRingbufferReceive(usb.m_bulk_rx_buffer, &item_size, portMAX_DELAY));
        if (item == nullptr || item_size == 0)
        {
            continue;
        }
        if (item_size > DAP_PACKET_SIZE)
        {
            // EP 64B 下不可能发生；防御性丢弃
            ESP_LOGW(TAG, "Oversized DAP request (%u bytes), dropped", static_cast<unsigned>(item_size));
            vRingbufferReturnItem(usb.m_bulk_rx_buffer, item);
            continue;
        }
        memcpy(dap_request, item, item_size);
        vRingbufferReturnItem(usb.m_bulk_rx_buffer, item);

        if (!tud_vendor_n_mounted(ITF_NUM_DAP))
        {
            continue; // 主机未枚举完成前不处理
        }

        const u32 n = DAP_ExecuteCommand(dap_request, dap_response);
        u16 resp_len = static_cast<u16>(n & 0xFFFFU);
        if (resp_len > DAP_PACKET_SIZE)
        {
            // 仅 0x7F 批量命令可触发；与上游 [DAP_PACKET_COUNT][DAP_PACKET_SIZE] 缓冲同语义
            ESP_LOGW(TAG, "DAP response truncated (%u -> %u)", resp_len, static_cast<unsigned>(DAP_PACKET_SIZE));
            resp_len = DAP_PACKET_SIZE;
        }
        dap_send_response(resp_len);
    }
}

} // namespace

void tud_vendor_rx_cb(u8 idx, const u8 *buffer, u16 buffer_size)
{
    (void)buffer;
    (void)buffer_size;
    if (idx != ITF_NUM_DAP)
    {
        return;
    }

    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：回调不携带数据，
    // 必须从内部 RX FIFO 读取。tud_vendor_n_read 仅在该模式下可用。
    auto &usb = mlink::Application::get_instance().usb();
    u8 tmp[CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE]; // 512B，≥ FIFO 64B
    u32 n;
    while ((n = tud_vendor_n_read(idx, tmp, sizeof(tmp))) > 0)
    {
        if (xRingbufferSend(usb.m_bulk_rx_buffer, tmp, n, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Ring buffer full, dropped %d bytes", (int)n);
            break;
        }
    }
}

namespace mlink
{
CmsisDapUsb::CmsisDapUsb()
{
    m_string_descriptors[0] = LANGID;
    m_string_descriptors[1] = "MeguminLink";                     // Manufacturer
    m_string_descriptors[2] = "CMSIS-DAP on " CONFIG_IDF_TARGET; // Product（必须含 "CMSIS-DAP"）
    m_string_descriptors[3] = m_serial;                          // Serial（init 时填充）
    m_string_descriptors[4] = "CMSIS-DAP v2";                    // Interface

    constexpr size_t RINGBUF_SIZE = CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE * CONFIG_MEGUMINLINK_USB_PACKET_BUFFER_DEPTH;

    m_bulk_rx_buffer = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
}

CmsisDapUsb::~CmsisDapUsb()
{
    vRingbufferDelete(m_bulk_rx_buffer);
    m_bulk_rx_buffer = nullptr;

    vTaskDelete(m_dap_task);
    m_dap_task = nullptr;
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

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    ret = uart_bridge_init();
    __check("Failed to initialize USB CDC ACM interface");
#endif

    // 创建DAP处理任务
    xTaskCreate(dap_task, "CMSIS-DAP", 2048, this, 0, &m_dap_task);

    m_initialized = true;
    return ESP_OK;

err:
    return ret;
}
} // namespace mlink
