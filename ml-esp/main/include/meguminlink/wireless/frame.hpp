#pragma once
#ifndef __MEGUMINLINK_WIRELESS_FRAME_HPP__
#define __MEGUMINLINK_WIRELESS_FRAME_HPP__

#include "esp_err.h"
#include "esp_now.h"
#include "meguminlink/common.hpp" // IWYU pragma: keep

#include <cstring>

namespace mlink
{
/**
 * 链路层业务包：配对控制帧与配置中继载荷，经 ESP-NOW 直发（尽力而为）。
 *
 * 帧格式：[type:1B][帧体]。可靠传输、序号与确认由 meaproto 层负责，本层不重传。
 */
inline constexpr size_t PACKET_MAX_SIZE = 512;

/**
 * 串口数据的来源端口。接收方按同一方向把内容写进对侧端口：来自主机的写本机
 * UART TX，来自目标板的写本机 CDC。两端因此不需要约定谁是 PC 侧、谁是目标侧。
 */
enum class UartSource : u8
{
    Host,   /**< 来自主机侧 USB CDC */
    Target, /**< 来自目标侧 UART */
};

namespace payload
{
struct Probe
{
};

struct PairingRequest
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};

struct PairingAccepted
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};

/**
 * 转发到对端执行的 USB 控制请求（USB SETUP 包原样 + DATA OUT 载荷）。
 *
 * 主机只能直连 USB 侧，无 USB 侧（接收端）的设置只能靠这条通路下发：发送端
 * 把控制请求原样打包给对端，对端用同一张命令表执行，两端语义因此不会漂移，
 * 新增设置项也不需要新开一个 PacketType。
 *
 * 数据段：[len:2 大端][bmRequestType:1][bRequest:1][wValue:2 大端]
 * [wIndex:2 大端][wLength:2 大端][载荷:len B]。len 是载荷字节数，wLength
 * 保持 USB 语义原样透传。载荷为指针视图：data 指向调用方缓冲，零拷贝。
 */
struct RelayedUsbControl
{
    u16 len; /**< 载荷字节数（data[0..len-1]） */
    u8 bm_request_type;
    u8 b_request;
    u16 w_value;
    u16 w_index;
    u16 w_length;
    const u8 *data;
};

/** RelayedUsbControl 的固定部分长度（SETUP 包 8B） */
inline constexpr size_t RELAYED_CONTROL_HEADER_SIZE = 8;
/** RelayedUsbControl 可携带的载荷上限（覆盖配置通道现有 OUT 命令的载荷） */
inline constexpr size_t MAX_RELAYED_CONTROL_PAYLOAD = 256;
} // namespace payload

enum class PacketType
{
    Probe,
    PairingRequest,    /**< 配对请求，对方必须回应一个 PairingAccepted 才能完成配对 */
    PairingAccepted,   /**< 接受配对请求，作为对 PairingRequest 的回应 */
    RelayedUsbControl, /**< 转发到对端执行的 USB 控制请求（无 USB 侧的设置下发通路） */
};

union PacketPayload {
    payload::Probe probe;
    payload::PairingRequest pairing_request;
    payload::PairingAccepted pairing_accepted;
    payload::RelayedUsbControl relayed_usb_control;
};

class Packet
{
  public:
    Packet();

    /**
     * 从业务包字节反序列化。
     * @param storage     中继载荷的数据存储（RelayedUsbControl 拷到这里，
     *                    其 data 指针指向本存储）
     * @param storage_cap  存储容量（≥ MAX_RELAYED_CONTROL_PAYLOAD）
     */
    esp_err_t deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap);

    /** 序列化成业务包字节 */
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的业务包长度 */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;
};

/** 一个已接收的业务包，附带 ESP-NOW 源 MAC */
struct ReceivedPacket
{
    Packet packet;
    u8 src_mac[ESP_NOW_ETH_ALEN];

    ReceivedPacket() = default;
};

} // namespace mlink

#endif
