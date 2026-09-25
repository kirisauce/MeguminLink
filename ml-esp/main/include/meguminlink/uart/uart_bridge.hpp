#pragma once
#ifndef __MEGUMINLINK_UART_BRIDGE_HPP__
#define __MEGUMINLINK_UART_BRIDGE_HPP__

#include "meguminlink/common.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "sdkconfig.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace mlink
{
/**
 * 本机串口 ↔ 对侧的桥：两端共用一份实现。
 *
 * UART 驱动安装、事件处理、两条任务与线编码翻译对 S3 与 C3 完全一致，差异只有
 * "对侧端口是什么"：有 USB 的一端对侧是 CDC（且 Local 模式下本机串口与 CDC 直通、
 * 不经过无线），无 USB 的一端对侧只有无线。分歧点少且编译期已知，因此就地用
 * CONFIG_MEGUMINLINK_USB_SUPPORT 表达，不再维护两份各自演化的副本。
 *
 * 两个方向的来源与阻塞特性都不同，因此各用一条任务：
 *   - rx：等 UART 驱动的事件队列（m_uart_events），读完按模式转给对侧；
 *   - tx：等对侧投递的待写帧（m_inbound_frames），写本机端口。
 * 写 UART 会阻塞（uart_write_bytes 在 TX 环形缓冲满时一直等），所以 tx 必须
 * 独立成任务——它绝不能落在 esp_wifi 任务的收帧回调里。
 *
 * 两个队列都是本对象的内部实现，由 init() 自建：对侧只经 push_inbound() 投递，
 * 不接触队列本身，也就不必操心它们的创建时机。
 */
class UartBridge
{
  public:
    /** 安装 UART 驱动、建立两个队列并创建两条任务（幂等） */
    esp_err_t init();

    /**
     * 对侧来了数据、要写进本机端口。由收帧回调（esp_wifi 任务上下文）调用，
     * 只做一次拷贝入队：写端口的动作在 tx 任务里做。队列满时丢帧并计数。
     */
    void push_inbound(const u8 *data, size_t len, UartSource source);

    /**
     * UART 路径：写本机 UART TX。来源有两种：对侧无线帧带来的数据，以及 Local
     * 模式下主机从 CDC 发来的数据。尽力而为：TX 环形缓冲满时丢弃多余字节并告警。
     */
    void send_uart_data(const u8 *data, size_t len);

    /**
     * 应用线编码（参数为 CDC 枚举原值）。各参数独立校验：驱动不支持的值
     * （数据位 16、Mark/Space 校验等）告警并跳过该项，其余照常应用。
     * 调用方：Local 模式下的主机 SET_LINE_CODING，以及无线来的 UartLineCoding 帧。
     */
    void configure(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

  private:
    /** 一条待写入本机端口的数据：来源决定落到 UART TX 还是主机 CDC */
    struct InboundFrame
    {
        UartSource source;
        u16 len;
        u8 data[MAX_UART_PAYLOAD_SIZE];
    };

    /** 本机 UART → 对侧：等 m_uart_events */
    static void rx_task_fn(void *arg);
    /** 对侧 → 本机端口：等 m_inbound_frames */
    static void tx_task_fn(void *arg);

    bool m_initialized = false;
    /** UART 驱动投递的事件（UART_DATA / 溢出 / 错误），rx 任务消费 */
    QueueHandle_t m_uart_events = nullptr;
    /** 对侧投递的待写帧，tx 任务消费 */
    QueueHandle_t m_inbound_frames = nullptr;
    TaskHandle_t m_rx_task = nullptr;
    TaskHandle_t m_tx_task = nullptr;
};
} // namespace mlink

#endif
