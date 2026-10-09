#include "meguminlink/usb/cdc_acm.hpp"

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE

#include "meguminlink/app.hpp"

#include <esp_log.h>
#include <tinyusb_cdc_acm.h>

namespace
{
using namespace mlink;

constexpr char TAG[] = "mlink::cdc_acm";

/** CDC 回调的接收缓冲（TinyUSB 任务上下文） */
u8 s_rx_buffer[256];

// CDC RX 回调运行在 TinyUSB 任务上下文（tinyusb_task 派发）：只 read + 分流。
// Local 模式：写本机 UART TX；Wireless 模式：打包为 UartFromHost 帧发给配对
// peer（尽力而为，未配对丢弃）。
void cdcacm_rx_callback(int interface, [[maybe_unused]] cdcacm_event_t *event)
{
    size_t rx_size = 0;
    const esp_err_t ret =
        tinyusb_cdcacm_read(static_cast<tinyusb_cdcacm_itf_t>(interface), s_rx_buffer, sizeof(s_rx_buffer), &rx_size);
    if (ret != ESP_OK || rx_size == 0)
    {
        ESP_LOGW(TAG, "CDC read failed: %s", esp_err_to_name(ret));
        return;
    }

    auto &app = Application::get_instance();
    if (app.mode() == Mode::Wireless)
    {
        app.forward_uart_data_from_port(s_rx_buffer, rx_size, UartSource::Host);
        app.config_channel().add_uart_wireless_tx(static_cast<u16>(rx_size));
        return;
    }

    // Local：CDC 与本地 UART 直通（写入量统计与失败告警都在桥里）
    app.uart_bridge().send_uart_data(s_rx_buffer, rx_size);
}

// CDC 行编码回调（主机 SET_LINE_CODING），同样运行在 TinyUSB 任务上下文。
// uart_set_* 为任务安全 API（内部持互斥锁），可直接调用。
void cdcacm_line_coding_callback(int interface, cdcacm_event_t *event)
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

    auto &app = Application::get_instance();

    // 线编码先记给状态显示，再按模式分流：Wireless 下主机改参数同样要反映出来
    app.config_channel().set_uart_line_coding(lc->bit_rate, lc->data_bits, lc->stop_bits, lc->parity);

    if (app.mode() == Mode::Wireless)
    {
        // Wireless：线编码无线同步给对端 UART（PC 改参数 → 目标侧实时跟随）
        app.route_line_coding(lc->bit_rate, lc->data_bits, lc->stop_bits, lc->parity);
        return;
    }

    // Local：直接配置本机 UART
    app.uart_bridge().configure(lc->bit_rate, lc->data_bits, lc->stop_bits, lc->parity);
}
} // namespace

esp_err_t CdcAcm::init()
{
    tinyusb_config_cdcacm_t cfg = {};
    cfg.callback_rx = cdcacm_rx_callback;
    cfg.callback_line_coding_changed = cdcacm_line_coding_callback;
    return tinyusb_cdcacm_init(&cfg);
}

size_t CdcAcm::send_uart_data(const u8 *data, size_t len)
{
    // write_queue 同步拷贝入 CDC TX FIFO，满时返回已入队数；flush 非阻塞发起 IN
    // 传输（短数据必须显式 flush 才送得出去）。尽力而为：FIFO 满丢弃多余字节并告警。
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
    return queued;
}

bool CdcAcm::host_open() const
{
    // DTR 置位且总线就绪才算"主机打开了串口"：断开后 tud_ready() 为 false，
    // 显示不会停在过期值上
    return tud_cdc_n_connected(TINYUSB_CDC_ACM_0);
}

#endif
