#include "meguminlink/uart/uart_bridge.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/common.hpp"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/usb.hpp"
#endif

#include <cstring>

#include <driver/uart.h>
#include <esp_check.h>
#include <esp_log.h>
#include <sdkconfig.h>

static constexpr char TAG[] = "mlink::uart";

namespace
{
// UART 桥参数：两端共用同一套 Kconfig（见 Kconfig.projbuild，放在 USB 条件之外，
// 非 USB 端也能配置）。
constexpr uart_port_t UART_NUM = static_cast<uart_port_t>(UART_NUM_0 + CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);
constexpr int UART_TX_IO = CONFIG_MEGUMINLINK_UART_BRIDGE_TX_IO;
constexpr int UART_RX_IO = CONFIG_MEGUMINLINK_UART_BRIDGE_RX_IO;
constexpr int UART_BAUD_RATE = CONFIG_MEGUMINLINK_UART_BRIDGE_BAUD_RATE;
constexpr int RX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_RX_BUFFER_SIZE;
constexpr int TX_BUF_SIZE = CONFIG_MEGUMINLINK_UART_TX_BUFFER_SIZE;
constexpr int QUEUE_SIZE = CONFIG_MEGUMINLINK_UART_EVENT_QUEUE_SIZE;

// 单次读取缓冲：一条 UART 消息的数据上限
constexpr size_t READ_CHUNK = mlink::UART_INBOUND_FRAME_MAX;

// 写 UART 撞上缓冲满时的让出次数上限（每次 1 tick，约 100ms）：115200 波特率
// 下写满 256B 缓冲 ≈ 22ms，留足余量；超过即认为对端不消费，丢弃兜底。
constexpr u32 UART_WRITE_STALL_LIMIT = 100;
} // namespace

namespace mlink
{
esp_err_t UartBridge::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    static_assert(RX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));
    static_assert(TX_BUF_SIZE == 0 || TX_BUF_SIZE > UART_HW_FIFO_LEN(UART_NUM));

    esp_err_t ret = uart_driver_install(UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, QUEUE_SIZE, &m_uart_events, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_install(UART%d, ...) failed", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM);

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

    // 对侧投递的待写帧：深度只吸收调度抖动，不承担吞吐（限速在对侧准入里做）
    m_inbound_frames = xQueueCreate(CONFIG_MEGUMINLINK_UART_INBOUND_QUEUE_DEPTH, sizeof(InboundFrame));
    if (m_inbound_frames == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create inbound frame queue");
        return ESP_ERR_NO_MEM;
    }

    // 优先级 5：DAP 执行器（优先级 24）运行期间两条桥任务都被抢占，UART RX 靠
    // 硬件 FIFO + 环形缓冲兜底（256B @115200 ≈ 22ms 余量，单条 DAP 命令执行为
    // us~ms 级）。任务平时阻塞在各自的队列上，因此不占 CPU。
    if (xTaskCreate(rx_task_fn, "uart_rx", 4096, this, 5, &m_rx_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UART RX bridge task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tx_task_fn, "uart_tx", 4096, this, 5, &m_tx_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UART TX bridge task");
        return ESP_ERR_NO_MEM;
    }

    m_initialized = true;
    return ESP_OK;
}

void UartBridge::rx_task_fn(void *arg)
{
    // 方向：本机 UART → 对侧。只等驱动的事件队列。
    auto *self = static_cast<UartBridge *>(arg);
    static u8 buffer[READ_CHUNK];
    uart_event_t evt;

    while (true)
    {
        if (xQueueReceive(self->m_uart_events, &evt, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (evt.type)
        {
        case UART_DATA: {
            // 一次读尽：UART_DATA 只在数据到达时触发一次，若一次读不完（环形缓冲
            // 大于单次读取量），剩余字节不会再有事件，必须在这里读完。
            while (true)
            {
                const int len = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), 0);
                if (len <= 0)
                {
                    break;
                }
                const size_t n = static_cast<size_t>(len);
                auto &app = Application::get_instance();

#if CONFIG_MEGUMINLINK_USB_SUPPORT && CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
                if (app.mode() == Mode::Local)
                {
                    // Local：本机串口与主机 CDC 直通，不经过无线
                    app.config_channel().add_uart_rx(static_cast<u16>(app.usb().cdc().send_uart_data(buffer, n)));
                    continue;
                }
#endif
                // 无线：本机目标串口的字节交给对端 CDC
                app.forward_uart_data_from_port(buffer, n, UartSource::Target);
                app.config_channel().add_uart_wireless_tx(static_cast<u16>(n));
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL: {
            ESP_LOGW(TAG, "UART RX overflow, flushing");
            uart_flush_input(UART_NUM);
            xQueueReset(self->m_uart_events);
            break;
        }
        default: {
            break; // 其余事件（奇偶校验错误等）忽略
        }
        }
    }
}

void UartBridge::push_inbound(const u8 *data, size_t len, UartSource source)
{
    // 收帧回调（esp_wifi 任务）调用：只拷贝入队。写端口的动作在 tx 任务里做，
    // 留在这里会把整个收发栈钉住（对端随即收不到确认，两端一起停摆）。
    if (len == 0)
    {
        return;
    }

    InboundFrame frame = {};
    frame.source = source;
    if (len > sizeof(frame.data))
    {
        len = sizeof(frame.data);
    }
    frame.len = static_cast<u16>(len);
    std::memcpy(frame.data, data, len);

    // 队列满即丢：深度只吸收调度抖动，不承担吞吐（限速在对侧的出向准入里做）
    (void)xQueueSend(m_inbound_frames, &frame, 0);
}

void UartBridge::tx_task_fn(void *arg)
{
    // 方向：对侧 → 本机端口。单独一条任务的唯一理由是 uart_write_bytes 在 TX
    // 环形缓冲满时会一直等——它绝不能落在 esp_wifi 任务里，收帧回调只做入队。
    auto *self = static_cast<UartBridge *>(arg);
    InboundFrame frame = {};

    while (true)
    {
        if (xQueueReceive(self->m_inbound_frames, &frame, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (frame.source == UartSource::Host)
        {
            self->send_uart_data(frame.data, frame.len);
            continue;
        }

#if CONFIG_MEGUMINLINK_USB_SUPPORT && CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
        // 来自目标侧方向的帧写本机 CDC（无 USB 或 CDC 接口未编译的一端没有落点，丢弃）
        auto &app = Application::get_instance();
        app.config_channel().add_uart_wireless_rx(
            static_cast<u16>(app.usb().cdc().send_uart_data(frame.data, frame.len)));
#endif
    }
}

void UartBridge::send_uart_data(const u8 *data, size_t len)
{
    // uart_write_bytes 是非阻塞的：TX 环形缓冲满时只写进一部分就返回（IDF 内部
    // uart_tx_all 的 ticks_to_wait 是 0）。tx 任务独立出来本就是为了能在这里等，
    // 所以循环把剩下的写完；写不动就让出，长时间排不空才丢弃兜底。
    size_t done = 0;
    u32 stalls = 0;
    while (done < len)
    {
        const int written = uart_write_bytes(UART_NUM, data + done, len - done);
        if (written < 0)
        {
            ESP_LOGW(TAG, "UART write failed: %s", esp_err_to_name(written));
            break;
        }
        if (written == 0)
        {
            if (++stalls > UART_WRITE_STALL_LIMIT)
            {
                ESP_LOGW(TAG, "UART TX stalled, dropped %u of %u bytes", static_cast<unsigned>(len - done),
                         static_cast<unsigned>(len));
                break;
            }
            vTaskDelay(1);
            continue;
        }
        done += static_cast<size_t>(written);
    }

    // 统计按当前模式落口径：Local 是本机 UART TX 引脚的发出量，
    // Wireless 是空口写回本机 UART 的量。只计真正写进去的部分。
    auto &app = Application::get_instance();
    if (app.mode() == Mode::Local)
    {
        app.config_channel().add_uart_tx(static_cast<u16>(done));
    }
    else
    {
        app.config_channel().add_uart_wireless_rx(static_cast<u16>(done));
    }
}

void UartBridge::configure(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    // 各参数独立校验：驱动不支持的值（数据位 16、Mark/Space 校验等）告警并跳过
    // 该项，其余照常应用。参数是 CDC 枚举原值，此处按数值判断，不引 tinyusb 头。

    // 波特率：0 表示"无偏好"（部分主机对虚拟串口发 0），跳过；
    // 驱动对 0 及不可达波特率返回 ESP_FAIL，告警后保留旧值
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

    // 校验位（CDC 枚举）：0=无 1=奇 2=偶 3=Mark 4=Space；驱动不支持后两者
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

    // 丢弃按旧参数采样的残余 RX 字节（FIFO + 环形缓冲）。队列里残留的 UART_DATA
    // 事件无害：桥任务随后读到 0 字节，被读尽循环直接结束，无需 xQueueReset。
    // 无条件冲刷：参数未变时为空操作，不丢有效数据。
    uart_flush_input(UART_NUM);

    ESP_LOGI(TAG, "UART%d line coding: %lu baud, %u data bits, stop %u, parity %u", CONFIG_MEGUMINLINK_UART_BRIDGE_NUM,
             static_cast<unsigned long>(baud), data_bits, stop_bits, parity);
}
} // namespace mlink
