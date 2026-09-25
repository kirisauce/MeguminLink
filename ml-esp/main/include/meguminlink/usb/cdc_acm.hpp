#pragma once
#ifndef __MEGUMINLINK_CDC_ACM_HPP__
#define __MEGUMINLINK_CDC_ACM_HPP__

#include <esp_err.h>

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

namespace mlink
{
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
/**
 * UART 透传的 CDC ACM 接口：USB 侧的那一端。
 *
 * 只管这一段的接口安装与收发。数据进来之后走本机串口还是无线、本机数据怎么送回
 * 主机，由调用方按模式决定（见 CmsisDapUsb 的回调与 UartBridge），本对象不判断模式。
 * 接口描述符不在这里：那是 USB 设备枚举的职责（见 UsbDescriptors）。
 */
class CdcAcm
{
  public:
    /** 安装 CDC ACM 接口并接上收发回调（须在 TinyUSB 驱动安装之后调用） */
    esp_err_t init();

    /**
     * CDC 路径：写主机 CDC TX（尽力而为：FIFO 满时丢弃多余字节并告警；短数据
     * 必须显式 flush 才送得出去）。统计落到哪个计数器由调用方决定，所以这里
     * 只返回写入量。
     *
     * @return 实际入队字节数
     */
    size_t send_uart_data(const u8 *data, size_t len);

    /**
     * 主机是否已打开 CDC ACM 串口（TinyUSB 的 DTR，且总线就绪）。
     * 实时查询不缓存：拔插与重新枚举后不会留下过期的"已打开"。
     */
    bool host_open() const;
};
#endif
} // namespace mlink

#endif
