#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"                    // IWYU pragma: export
#include "esp_wifi.h"                   // IWYU pragma: export
#include "meguminlink/common.hpp"       // IWYU pragma: keep
#include "meguminlink/dap_executor.hpp" // DAP_MAX_REQUEST_SIZE / DAP_MAX_RESPONSE_SIZE（执行器缓冲）

#include <freertos/FreeRTOS.h> // QueueHandle_t / TaskHandle_t
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstring> // memcpy（ReceivedPacket 拷贝）

namespace mlink
{
/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/**
 * DapCommand/DapResponse 帧格式：[type:1B][len:2B 大端][data:len B]。
 * 数据段上限 = CMSIS-DAP 单包大小（DAP_PACKET_SIZE=64B，USB EP 64B 单次
 * 传输即一个请求/响应包）。0x7F 批量命令的超大响应由执行器丢弃
 * （见 dap_executor.cc），不走无线帧——因此 Packet 结构保持小巧，
 * 调用方可放心放在任务栈上。
 */
inline constexpr size_t DAP_FRAME_HEADER_SIZE = 3;
inline constexpr size_t MAX_DAP_PAYLOAD_SIZE = DAP_MAX_REQUEST_SIZE; // 64B，见 dap_executor.hpp

/**
 * UartData 帧格式与 DAP 帧一致：[type:1B][len:2B 大端][data:len B]。
 * payload 上限 = 200B：ESP-NOW 单帧数据段上限 250B，扣 3B 帧头后取 200B
 * 留余量。串口是流式数据，64B 单包语义（DAP）对吞吐是瓶颈——115200
 * 波特率下 200B 帧约 58 帧/秒，突发日志余量远大于 64B 帧。
 */
inline constexpr size_t UART_FRAME_HEADER_SIZE = 3;
inline constexpr size_t MAX_UART_PAYLOAD_SIZE = 200;

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
 * DAP 命令/响应为"指针视图"：Packet 只是序列化包装层，不内嵌数据。
 * 发送时 data 指向调用方命令/响应缓冲；接收（反序列化）时由调用方提供
 * 存储（见 ReceivedPacket::storage），data 指向该存储。数据段上限
 * MAX_DAP_PAYLOAD_SIZE = DAP_PACKET_SIZE（64B，USB 单包），Packet 因此
 * 保持小巧，可放心放任务栈上。
 */
struct DapCommand
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 命令字节流（指向调用方存储） */
};

/** 与 DapCommand 同构：接收端执行 DAP 命令后的响应视图 */
struct DapResponse
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 响应字节流（指向调用方存储） */
};

/**
 * 无线转发的串口数据（尽力而为，不重传）：PC 侧 CDC ↔ 目标侧 UART 双向。
 * 角色天然区分数据方向：发送端（USB 侧）把 CDC RX 打包发送，接收端
 * （无 USB 侧）把收到的帧写入 UART TX；反向同理。数据段上限
 * MAX_UART_PAYLOAD_SIZE = 200B，与 DapCommand 同为指针视图。
 */
struct UartData
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 串口字节流（指向调用方存储） */
};

/**
 * 无线同步的 UART 线编码参数（PC 端 CDC SET_LINE_CODING → 目标侧 UART）。
 * 帧数据段 7B：baud 4B 大端 + data_bits/stop_bits/parity 各 1B。参数传 CDC
 * 枚举原值，目标侧翻译为 UART 驱动参数（目标侧无 tinyusb 头，按数值判断）。
 */
struct UartLineCoding
{
    u32 baud;     /**< 波特率（0 = 无偏好，跳过） */
    u8 data_bits; /**< CDC 枚举：5/6/7/8 */
    u8 stop_bits; /**< CDC 枚举：0=1, 1=1.5, 2=2 */
    u8 parity;    /**< CDC 枚举：0=None, 1=Odd, 2=Even, 3=Mark, 4=Space */
};
} // namespace payload

enum class PacketType
{
    Probe,
    PairingRequest,  /** 配对请求，对方必须回应一个`PairingAccepted`才能完成配对 */
    PairingAccepted, /** 接受配对请求，作为对`PairingRequest`的回应 */
    DapCommand,      /** 无线转发的 DAP 命令载荷（USB 收到的 CMSIS-DAP 请求原样打包） */
    DapResponse,     /** 无线回传的 DAP 响应载荷（接收端执行 DAP 命令的结果原样打包） */
    UartData,        /** 无线转发的串口数据载荷（PC↔目标 UART 双向，尽力而为） */
    UartLineCoding,  /** 无线同步的 UART 线编码载荷（PC 端 SET_LINE_CODING → 目标侧） */
};

union PacketPayload {
    payload::Probe probe;
    payload::PairingRequest pairing_request;
    payload::PairingAccepted pairing_accepted;
    payload::DapCommand dap_command;
    payload::DapResponse dap_response;
    payload::UartData uart_data;
    payload::UartLineCoding uart_line_coding;
};

class Packet
{
  public:
    Packet();

    /**
     * 从无线帧反序列化。
     * @param storage      payload 数据存储（DapCommand/DapResponse 的数据
     *                     拷贝到这里，其 data 指针指向本存储）
     * @param storage_cap  存储容量（≥ MAX_DAP_PAYLOAD_SIZE）
     */
    esp_err_t deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap);
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的帧长度，用于发送时告知 ESP-NOW */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;
};

/**
 * 一帧已接收的协议包，附带 ESP-NOW 源 MAC。
 * storage 为反序列化的 payload 数据存储（命令/响应字节流），
 * 生命周期与本对象一致（主循环消费完才销毁）。
 * 注意：本对象会被值拷贝入接收队列（vector），拷贝时必须把
 * payload 的 data 指针重绑到拷贝后的 storage（见 rebind_data_ptr）。
 */
struct ReceivedPacket
{
    Packet packet;
    u8 src_mac[ESP_NOW_ETH_ALEN];
    // 按最大的 payload 存储预留：UartData（200B）> DapCommand/DapResponse（64B）。
    // UartData 在 recv_cb 内直通处理（不排队），storage 仅为反序列化中转。
    u8 storage[MAX_UART_PAYLOAD_SIZE];

    ReceivedPacket() = default;

    ReceivedPacket(const ReceivedPacket &other)
    {
        packet = other.packet;
        std::memcpy(src_mac, other.src_mac, ESP_NOW_ETH_ALEN);
        std::memcpy(storage, other.storage, sizeof(storage));
        rebind_data_ptr();
    }

    ReceivedPacket &operator=(const ReceivedPacket &other)
    {
        if (this != &other)
        {
            packet = other.packet;
            std::memcpy(src_mac, other.src_mac, ESP_NOW_ETH_ALEN);
            std::memcpy(storage, other.storage, sizeof(storage));
            rebind_data_ptr();
        }
        return *this;
    }

  private:
    /** 反序列化后 payload 数据总在 storage[0..len)，拷贝后重绑指针 */
    void rebind_data_ptr()
    {
        switch (packet.packet_type)
        {
        case PacketType::DapCommand:
            packet.payload.dap_command.data = storage;
            break;
        case PacketType::DapResponse:
            packet.payload.dap_response.data = storage;
            break;
        case PacketType::UartData:
            packet.payload.uart_data.data = storage;
            break;
        default:
            break;
        }
    }
};

/**
 * 无线链路：ESP-NOW 传输 + peer 管理 + 无线侧 DAP 执行接线，状态全在对象内。
 * 与 usb.cc 的 dap_task（USB 侧执行接线）对称：无线传输层负责把收到的
 * DapCommand 交给公共执行器（dap_executor_run）执行，结果以 DapResponse
 * 帧回发给来源；发送端（USB 侧）经 forward_dap_command 转发并等待响应。
 * 实例由 Application 持有（m_wireless_link），与 m_usb/m_pairing 同模式。
 */
class WirelessLink
{
  public:
    /** 初始化 WiFi/ESP-NOW、登记广播 peer，并创建无线侧 DAP 执行器（幂等） */
    esp_err_t init();

    /** 向指定对端（可为广播地址）发送一帧协议包 */
    esp_err_t send(const u8 *dest_mac, const Packet &packet);

    /** 打包一条 DAP 命令/响应（原样字节流）为对应帧并发送给对端 */
    esp_err_t send_dap_command(const u8 *dest_mac, const u8 *data, size_t len);
    esp_err_t send_dap_response(const u8 *dest_mac, const u8 *data, size_t len);

    /**
     * 打包一段串口数据为 UartData 帧并发送给对端（尽力而为，不重传）。
     * @param len 数据长度（> MAX_UART_PAYLOAD_SIZE 时拒绝）
     */
    esp_err_t send_uart_data(const u8 *dest_mac, const u8 *data, size_t len);

    /**
     * 打包线编码参数为 UartLineCoding 帧并发送给对端（尽力而为，不重传）。
     * 参数为 CDC 枚举原值（见 payload::UartLineCoding），目标侧翻译应用。
     */
    esp_err_t send_uart_line_coding(const u8 *dest_mac, u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    /**
     * 转发一条 DAP 命令给配对 peer 并等待其响应（Wireless 模式 USB→无线通路）。
     * 查找配对 peer、打包 DapCommand、等待 DapResponse 均在此完成；响应写入
     * resp 缓冲（容量 resp_cap，需 ≥ MAX_DAP_PAYLOAD_SIZE）。
     * @return ESP_OK=已收到响应；否则失败（未配对 / 发送失败 / 响应超时，
     *         命令已丢弃，PC 端 debugger 会自行超时重试）
     */
    esp_err_t forward_dap_command(const u8 *cmd, size_t len, u8 *resp, size_t resp_cap, u16 *resp_len);

    /**
     * 登记一个对端 peer（encrypt=false）。仅登记尚未存在于 peer list 的地址，
     * 重复调用是安全的。广播地址已由 init() 常驻登记，无需也不应重复登记。
     */
    esp_err_t peer_add(const u8 *peer_mac);

    /** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
    esp_err_t peer_remove(const u8 *peer_mac);

    /**
     * 投递一条无线接收到的 DAP 命令给执行器（异步入队，立即返回）。
     * @param src_mac 命令来源 MAC（响应回发地址，6 字节）
     * @param data    DAP 命令原样字节流
     * @param len     命令长度（> DAP_PACKET_SIZE 时拒绝，与 USB 路径一致）
     */
    esp_err_t dap_execute(const u8 *src_mac, const u8 *data, size_t len);

  private:
    /** ESP-NOW 接收/发送回调（esp_now 要求 C 函数指针，无 arg；经 Application 访问） */
    static void recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len);
    static void send_cb(const wifi_tx_info_t *send_info, esp_now_send_status_t send_status);

    /** 无线侧 DAP 执行器回调（arg = 本对象 this）与任务入口 */
    static bool dap_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg);
    static void dap_resp_sink(const u8 *data, u16 len, void *arg);
    static void dap_task_fn(void *arg);

    /** 设置单播 peer 的 ESP-NOW 发送速率（广播地址跳过） */
    esp_err_t set_peer_rate(const u8 *peer_mac);

    static constexpr u8 CHANNEL = 10; // ESP-NOW 固定信道

    bool m_initialized = false;
    /** 无线侧 DAP 执行器（命令队列 + 任务 + 缓冲，状态全在对象内） */
    QueueHandle_t m_dap_queue = nullptr;
    TaskHandle_t m_dap_task = nullptr;
    /** 最近一条命令的来源（响应回发地址）；仅执行任务线程读写 */
    u8 m_dap_src_mac[ESP_NOW_ETH_ALEN];
    /** 执行器命令/响应缓冲（不占任务栈，见 dap_executor.hpp） */
    u8 m_dap_request[DAP_MAX_REQUEST_SIZE];
    u8 m_dap_response[DAP_MAX_RESPONSE_SIZE];
};
} // namespace mlink

#endif
