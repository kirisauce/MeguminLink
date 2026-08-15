#pragma once
#ifndef __MEGUMINLINK_WIRELESS_UART_HPP__
#define __MEGUMINLINK_WIRELESS_UART_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#include <freertos/FreeRTOS.h> // QueueHandle_t / TaskHandle_t
#include <freertos/queue.h>
#include <freertos/task.h>

namespace mlink
{
/**
 * 目标侧（无 USB）无线串口桥：UART <-> ESP-NOW。
 *
 * 仅非 USB 端（如无线接收端 ESP32-C3）编译：UART RX 数据打包为 UartData
 * 帧发给配对 peer（PC 侧），收到 peer 发来的 UartData 帧写入 UART TX。
 * UART 参数（外设号/GPIO/波特率/缓冲）与 USB 侧本地桥共用同一套 Kconfig
 * （MEGUMINLINK_UART_BRIDGE_*，见 Kconfig.projbuild，非 USB 端也可配置）。
 *
 * 与无线 DAP 执行器（WirelessLink::dap_execute）对称：数据收发起点为
 * Application 统一入口（send_uart_data / on_uart_data），桥只负责
 * UART 驱动、事件任务与帧收发接线。
 */
class WirelessUartBridge
{
  public:
    /** 安装 UART 驱动并创建桥任务（幂等） */
    esp_err_t init();

    /**
     * 写 UART TX（收到无线 UartData 帧时由 recv_cb 直通调用，运行在
     * esp_wifi 任务上下文；uart_write_bytes 为任务安全 API，可直接调用）。
     * 尽力而为：TX 缓冲满时驱动内部丢弃并返回负值，此处仅告警。
     */
    void write_uart(const u8 *data, size_t len);

    /**
     * 应用无线同步的线编码参数（PC 端 CDC SET_LINE_CODING → 本机 UART，
     * recv_cb 直通调用）。参数为 CDC 枚举原值，翻译逻辑与
     * usb/uart_bridge.cc 的 line coding 回调对齐（目标侧无 tinyusb 头）。
     * 各参数独立校验：驱动不支持的值告警并跳过该项，其余照常应用。
     *
     * @param baud      波特率（0 = 无偏好，跳过）
     * @param data_bits CDC 枚举 5/6/7/8
     * @param stop_bits CDC 枚举 0=1, 1=1.5, 2=2
     * @param parity    CDC 枚举 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space
     */
    void configure(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

  private:
    /** 桥任务入口：UART 事件队列 → UART_DATA 读出 → 无线帧发送 */
    static void bridge_task_fn(void *arg);

    bool m_initialized = false;
    QueueHandle_t m_uart_queue = nullptr;
    TaskHandle_t m_uart_bridge_task = nullptr;
};
} // namespace mlink

#endif
