#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp"
#include "meguminlink/usb/cdc_acm.hpp"
#include "meguminlink/usb/usb_capability.hpp"
#include "meguminlink/usb/usb_descriptors.hpp"
#include "sdkconfig.h"

namespace mlink
{
/**
 * CMSIS-DAP USB 设备（复合设备）。
 *
 * 描述符集由 UsbDescriptors 持有与构建；DAP 命令处理在 usb.cc 的 dap_task 中。
 */
class CmsisDapUsb
{
  public:
    CmsisDapUsb();
    ~CmsisDapUsb();

    /**
     * 安装 TinyUSB 驱动、注册 CMSIS-DAP v2 描述符，并在 UART 透传接口开启时安装
     * CDC ACM 接口（见 CdcAcm）（幂等）。
     *
     * 本机串口的驱动安装与桥任务不在这里：那是 UartBridge 的职责，两端共用
     * 同一实现（见 meguminlink/uart/uart_bridge.hpp），USB 侧只提供 CDC 这一端。
     */
    esp_err_t init();

    /**
     * 启用/禁用整个 USB 设备（任务上下文调用）。
     *
     * disabled：tud_disconnect() 断开 USB 总线（主机端表现为拔线），
     * 并挂起 DAP 任务；enabled：tud_connect() 重新枚举并恢复任务。
     *
     * 注意：当前无调用方——USB 在 Local 与 Wireless 模式下都常驻启用
     * （Wireless 下 DAP 命令由 dap_task 无线转发，见 usb.cc），此接口保留
     * 以备将来按需启停（如省电、仅断开 CDC 等场景）。
     */
    void set_enabled(bool enabled);

    /** 当前生效的 USB 能力配置（BOS 与配置描述符据此构造） */
    const UsbCapabilitySettings &usb_capability() const
    {
        return m_usb_capability;
    }

    /** HID 接口当前是否对主机暴露（HID 兼容模式开启时才在配置描述符里） */
    bool hid_exposed() const
    {
        return m_usb_capability.hid_compat != 0;
    }

    /** DAP_Info 的产品名（含自定义设备名；无名字时为 "MeguminLink CMSIS-DAP"） */
    const char *dap_product_name() const
    {
        return m_descriptors.dap_product_name();
    }

    /** 描述符访问器：TinyUSB 安装与枚举回调（BOS / GET_URL / GET_DESCRIPTOR_SET）从这里取数据 */
    const UsbDescriptors &descriptors() const
    {
        return m_descriptors;
    }

    /**
     * 应用新的 USB 能力配置：断开总线 → 重建描述符（BOS、GET_URL、iLandingPage
     * 字符串与 USB 配置描述符）→ 重新连接，主机重新读取描述符后新配置才可见。
     *
     * 重建放在断开窗口内：此时主机已停止请求描述符，描述符缓冲与字符串内容
     * 不会被 TinyUSB 任务并发读取。NVS 落盘由调用方负责（见 app.cc）。
     */
    void apply_usb_capability(const UsbCapabilitySettings &settings);

    TaskHandle_t m_dap_task = nullptr;
    /**
     * DAP 命令队列（NOSPLIT ringbuffer，元素为 usb.cc 的 DapCommand）：
     * vendor bulk（CMSIS-DAP v2）与 HID（CMSIS-DAP v1）两条传输通道共用，
     * 由 dap_task 顺序消费——DAP 执行器不可重入，命令必须串行执行。
     */
    RingbufHandle_t m_dap_command_buffer;

    /**
     * 跨 USB 包重组缓冲：一条命令可能大于单个 USB 包，接收回调按命令
     * 自描述长度累积，攒够一条才入队。仅 TinyUSB 任务上下文读写。
     */
    u8 m_rx_accum[DAP_MAX_REQUEST_SIZE];
    size_t m_rx_accum_len = 0;

  public:
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /** UART 透传的 CDC ACM 接口（主机串口）：接口安装与收发都在 CdcAcm 里 */
    CdcAcm &cdc()
    {
        return m_cdc;
    }
#endif

  private:
    bool m_initialized = false;

    /**
     * USB 能力状态（构造时从 NVS 载入）。m_usb_capability.landing_page 同时是
     * iLandingPage 字符串描述符的内容来源，与描述符集一起下发给 UsbDescriptors。
     */
    UsbCapabilitySettings m_usb_capability;
    /** 描述符集（device / 字符串 / 配置 / HID 报告 / BOS），按上面的能力状态构建 */
    UsbDescriptors m_descriptors;

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /** UART 透传的 CDC ACM 接口（主机串口），见 cdc_acm.hpp */
    CdcAcm m_cdc;
#endif
};
} // namespace mlink

#endif
