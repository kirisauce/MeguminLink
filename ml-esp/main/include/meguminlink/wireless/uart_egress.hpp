#pragma once
#ifndef __MEGUMINLINK_UART_EGRESS_HPP__
#define __MEGUMINLINK_UART_EGRESS_HPP__

#include "meguminlink/common.hpp"

#include <freertos/FreeRTOS.h>

namespace mlink
{
/**
 * 无线出向的字节缓冲：主机串口的数据先落在这里，由链路按额度逐帧取走。
 *
 * 满了就不再接收，调用方据此停读端点，压力经 USB 反传给主机。
 */
class UartEgress
{
  public:
    /** 还能装多少字节 */
    size_t space();

    /** 入队，返回实际写入字节数（空间不足时可能小于 len） */
    size_t push(const u8 *data, size_t len);

    /** 看一眼队头但不消费 */
    size_t peek(u8 *dst, size_t max);

    /** 消费队头 len 字节（对应的 peek 已经发出去之后调用） */
    void consume(size_t len);

    /** 清空 */
    void flush();

  private:
    static constexpr size_t CAPACITY = 4096;

    u8 m_buffer[CAPACITY] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
    portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
};
} // namespace mlink

#endif
