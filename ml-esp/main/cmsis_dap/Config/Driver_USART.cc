/**
 * CMSIS-Driver USART 驱动（Driver_USART0）：ESP-IDF UART 之上的适配层，为
 * SWO UART 捕获（上游 SWO.c）提供接收通路。只收不发——SWO 是目标 → 探针的
 * 单向数据流。
 *
 * 顺着上游 CMSIS-Driver 的 ISR-compatible 设计：RX FIFO 中断里直接填当前块
 * 请求，满块在 ISR 内调回调（SWO.c 挂下一块），不引入任务与环形缓冲。请求
 * 状态走原子协议，任务侧（Receive/Abort/查询）与 ISR 两侧并发安全。中断挂
 * ESP_INTR_FLAG_IRAM 且整条捕获链 IRAM 化（含上游回调，见 SWO.c），NVS 写等
 * flash cache 禁用窗口内捕获不中断。
 */
#include "DAP_config.h"
#include "Driver_USART.h"

#if (SWO_UART != 0)

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <esp_private/uart_share_hw_ctrl.h>
#include <hal/uart_ll.h>
#include <soc/interrupts.h>
#include <atomic>
#include <cstdint>

namespace
{
constexpr char TAG[] = "mlink::swo_usart";

// UART0 是 console、UART1 默认归串口桥，SWO 用 UART2（S3 上为独立中断源）
constexpr uart_port_t SWO_UART_PORT = UART_NUM_2;
constexpr int SWO_UART_RXFIFO_FULL_THR = 120;
// TOUT 是流尾判定：FIFO 里有字节但 2 个字节时间无新数据时触发，兜住
// 不足 full threshold 的零散块（SWO.c 每次只挂 1 字节的请求起步）
constexpr uint8_t SWO_UART_RX_TOUT_BYTES = 2;
constexpr uint32_t SWO_UART_INTR_MASK = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                                        UART_INTR_RXFIFO_OVF | UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR;

// 块接收请求：busy 由原子操作独占（Receive 置位 / Abort 清除 / ISR 完成时清除
// 并回调）；count 只由 ISR 递增；gen 是请求代际，ISR 写回前校验，防止 Abort/
// 替换与填充并发时的跨请求污染。ISR 与 DAP 任务双核并发，全程无锁。
struct SwoUsart
{
    ARM_USART_SignalEvent_t cb = nullptr;
    intr_handle_t intr_handle = nullptr;
    bool installed = false;
    std::atomic<bool> rx_enabled{false};
    uint8_t *rx_buf = nullptr;
    uint32_t rx_num = 0;
    uint32_t rx_count = 0;
    std::atomic<bool> rx_busy{false};
    std::atomic<uint32_t> rx_gen{0};
};

// CMSIS-Driver ABI 编译期绑定驱动实例（SWO.c 引用 extern Driver_USART0），
// 运行时状态收在这份单例里
SwoUsart &swo_usart()
{
    static SwoUsart inst;
    return inst;
}

// FIFO → 当前请求：无请求或接收被禁时读空丢弃；满块清 busy 并回调
IRAM_ATTR void swo_uart_pump()
{
    uart_dev_t *hw = UART_LL_GET_HW(SWO_UART_PORT);
    auto &u = swo_usart();

    while (true)
    {
        const uint32_t gen = u.rx_gen.load(std::memory_order_acquire);
        if (!u.rx_busy.load(std::memory_order_acquire) || !u.rx_enabled.load(std::memory_order_relaxed))
        {
            uart_ll_rxfifo_rst(hw);
            return;
        }

        const uint32_t avail = uart_ll_get_rxfifo_len(hw);
        if (avail == 0U)
        {
            return;
        }
        const uint32_t want = u.rx_num - u.rx_count;
        const uint32_t n = (avail < want) ? avail : want;
        uart_ll_read_rxfifo(hw, u.rx_buf + u.rx_count, n);
        u.rx_count += n;

        if (u.rx_gen.load(std::memory_order_relaxed) != gen)
        {
            return; // 请求已被替换/Abort：放弃本批，新请求由下一次中断接手
        }
        if (u.rx_count < u.rx_num)
        {
            continue;
        }

        bool expected = true;
        const bool completed = u.rx_busy.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
        if (completed && u.cb != nullptr)
        {
            u.cb(ARM_USART_EVENT_RECEIVE_COMPLETE);
        }
    }
}

IRAM_ATTR void swo_uart_isr(void *arg)
{
    (void)arg;
    uart_dev_t *hw = UART_LL_GET_HW(SWO_UART_PORT);
    const uint32_t st = uart_ll_get_intsts_mask(hw) & SWO_UART_INTR_MASK;
    if (st == 0U)
    {
        return;
    }

    auto &u = swo_usart();
    if ((st & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) != 0U)
    {
        swo_uart_pump();
    }
    if ((st & UART_INTR_RXFIFO_OVF) != 0U && u.cb != nullptr)
    {
        u.cb(ARM_USART_EVENT_RX_OVERFLOW);
    }
    if ((st & UART_INTR_FRAM_ERR) != 0U && u.cb != nullptr)
    {
        u.cb(ARM_USART_EVENT_RX_FRAMING_ERROR);
    }
    if ((st & UART_INTR_PARITY_ERR) != 0U && u.cb != nullptr)
    {
        u.cb(ARM_USART_EVENT_RX_PARITY_ERROR);
    }
    uart_ll_clr_intsts_mask(hw, st);
}

int32_t swo_usart_Initialize(ARM_USART_SignalEvent_t cb_event)
{
    auto &u = swo_usart();
    if (!u.installed)
    {
        uart_config_t cfg = {};
        cfg.baud_rate = 115200;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_APB;
        if (uart_param_config(SWO_UART_PORT, &cfg) != ESP_OK)
        {
            ESP_LOGE(TAG, "uart_param_config failed");
            return ARM_DRIVER_ERROR;
        }

        // 共享时钟寄存器，按 IDF 约定在 RCC 临界区内开总线时钟
        HP_UART_BUS_CLK_ATOMIC()
        {
            uart_ll_enable_bus_clock(SWO_UART_PORT, true);
        }
        uart_ll_sclk_enable(UART_LL_GET_HW(SWO_UART_PORT));

        if (esp_intr_alloc(ETS_UART2_INTR_SOURCE, ESP_INTR_FLAG_IRAM, swo_uart_isr, nullptr, &u.intr_handle) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_intr_alloc failed");
            return ARM_DRIVER_ERROR;
        }
        u.installed = true;
    }
    u.cb = cb_event;
    return ARM_DRIVER_OK;
}

int32_t swo_usart_PowerControl(uint32_t state)
{
    uart_dev_t *hw = UART_LL_GET_HW(SWO_UART_PORT);
    auto &u = swo_usart();

    switch (state)
    {
    case ARM_POWER_OFF:
        uart_ll_disable_intr_mask(hw, SWO_UART_INTR_MASK);
        u.rx_enabled.store(false, std::memory_order_relaxed);
        u.rx_busy.store(false, std::memory_order_release);
        uart_ll_rxfifo_rst(hw);
        // 还原 SWO 引脚到默认输入态，断开 UART 信号矩阵连接
        gpio_reset_pin(DAP_SWO_GPIO);
        return ARM_DRIVER_OK;
    case ARM_POWER_FULL:
        // RX 接 SWO 引脚；TX/RTS/CTS 不用
        if (uart_set_pin(SWO_UART_PORT, UART_PIN_NO_CHANGE, DAP_SWO_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
            ESP_OK)
        {
            return ARM_DRIVER_ERROR;
        }
        uart_ll_rxfifo_rst(hw);
        return ARM_DRIVER_OK;
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

int32_t swo_usart_Uninitialize()
{
    // 驱动常驻（见文件头），这里与 PowerControl(OFF) 相同：断引脚、复位接收
    return swo_usart_PowerControl(ARM_POWER_OFF);
}

int32_t swo_usart_Send(const void *data, uint32_t num)
{
    (void)data;
    (void)num;
    return ARM_DRIVER_ERROR_UNSUPPORTED;
}

IRAM_ATTR int32_t swo_usart_Receive(void *data, uint32_t num)
{
    if (data == nullptr || num == 0U)
    {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    auto &u = swo_usart();
    if (u.rx_busy.exchange(true, std::memory_order_acq_rel))
    {
        return ARM_DRIVER_ERROR_BUSY;
    }
    u.rx_buf = static_cast<uint8_t *>(data);
    u.rx_num = num;
    u.rx_count = 0U;
    u.rx_gen.fetch_add(1, std::memory_order_acq_rel);
    return ARM_DRIVER_OK;
}

int32_t swo_usart_Transfer(const void *data_out, void *data_in, uint32_t num)
{
    (void)data_out;
    (void)data_in;
    (void)num;
    return ARM_DRIVER_ERROR_UNSUPPORTED;
}

uint32_t swo_usart_GetTxCount()
{
    return 0U;
}

uint32_t swo_usart_GetRxCount()
{
    auto &u = swo_usart();
    return u.rx_busy.load(std::memory_order_acquire) ? u.rx_count : 0U;
}

int32_t swo_usart_Control(uint32_t control, uint32_t arg)
{
    uart_dev_t *hw = UART_LL_GET_HW(SWO_UART_PORT);
    auto &u = swo_usart();

    switch (control)
    {
    // 8N1 在 Initialize 配置；本驱动只支持该形态，组合值只剩波特率语义
    case ARM_USART_MODE_ASYNCHRONOUS | ARM_USART_DATA_BITS_8 | ARM_USART_PARITY_NONE | ARM_USART_STOP_BITS_1:
        if (arg == 0U)
        {
            return ARM_DRIVER_ERROR_PARAMETER;
        }
        if (uart_set_baudrate(SWO_UART_PORT, arg) != ESP_OK)
        {
            return ARM_DRIVER_ERROR;
        }
        return ARM_DRIVER_OK;
    case ARM_USART_CONTROL_RX:
        if (arg != 0U)
        {
            u.rx_enabled.store(true, std::memory_order_relaxed);
            uart_intr_config_t cfg = {};
            cfg.intr_enable_mask = SWO_UART_INTR_MASK;
            cfg.rx_timeout_thresh = SWO_UART_RX_TOUT_BYTES;
            cfg.txfifo_empty_intr_thresh = 10;
            cfg.rxfifo_full_thresh = SWO_UART_RXFIFO_FULL_THR;
            if (uart_intr_config(SWO_UART_PORT, &cfg) != ESP_OK)
            {
                u.rx_enabled.store(false, std::memory_order_relaxed);
                return ARM_DRIVER_ERROR;
            }
        }
        else
        {
            // 先关中断再清状态，避免 ISR 并发
            uart_ll_disable_intr_mask(hw, SWO_UART_INTR_MASK);
            u.rx_enabled.store(false, std::memory_order_relaxed);
            uart_ll_rxfifo_rst(hw);
        }
        return ARM_DRIVER_OK;
    case ARM_USART_ABORT_RECEIVE:
        u.rx_busy.store(false, std::memory_order_release);
        return ARM_DRIVER_OK;
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

ARM_USART_STATUS swo_usart_GetStatus()
{
    ARM_USART_STATUS status = {};
    status.rx_busy = swo_usart().rx_busy.load(std::memory_order_acquire);
    return status;
}

int32_t swo_usart_SetModemControl(ARM_USART_MODEM_CONTROL control)
{
    (void)control;
    return ARM_DRIVER_ERROR_UNSUPPORTED;
}

ARM_USART_MODEM_STATUS swo_usart_GetModemStatus()
{
    return {};
}
} // namespace

// CMSIS-Driver ABI 的静态函数表：上游 SWO.c（C）按裸符号 extern 引用。保持
// 非 const 驻 .data（DRAM），使捕获链在 flash cache 禁用期间可继续取用——
// constexpr/const 会落入 flash 的 rodata，此处不可用
ARM_DRIVER_USART Driver_USART0 = {
    swo_usart_Initialize,
    swo_usart_Uninitialize,
    swo_usart_PowerControl,
    swo_usart_Send,
    swo_usart_Receive,
    swo_usart_Transfer,
    swo_usart_GetTxCount,
    swo_usart_GetRxCount,
    swo_usart_Control,
    swo_usart_GetStatus,
    swo_usart_SetModemControl,
    swo_usart_GetModemStatus,
};

#endif // (SWO_UART != 0)
