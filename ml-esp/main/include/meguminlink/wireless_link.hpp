#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"              // IWYU pragma: export
#include "esp_wifi.h"             // IWYU pragma: export
#include "meguminlink/common.hpp" // IWYU pragma: keep

namespace mlink
{
esp_err_t wireless_link_init();

/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/**
 * DapCommand 帧格式：[type:1B][len:2B 大端][data:len B]。
 * 数据段上限对齐 ESP-NOW v2.0 能力（ESP_NOW_MAX_DATA_LEN_V2 - 帧头，1467B）。
 * 第一版实际仅承载单条 DAP 命令（USB 单次请求 ≤ DAP_PACKET_SIZE = 64B），
 * 上限按无线链路能力预留，为后续批量命令（0x7F）与响应回传留出空间，
 * 避免将来为了大帧改写帧格式。
 */
inline constexpr size_t kDapFrameHeaderSize = 3;
inline constexpr size_t kMaxDapPayloadSize = ESP_NOW_MAX_DATA_LEN_V2 - kDapFrameHeaderSize; // 1467

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

struct DapCommand
{
    u16 len; /**< 有效数据长度（data[0..len-1]） */
    u8 data[kMaxDapPayloadSize];
};
} // namespace payload

enum class PacketType
{
    Probe,
    PairingRequest,  /** 配对请求，对方必须回应一个`PairingAccepted`才能完成配对 */
    PairingAccepted, /** 接受配对请求，作为对`PairingRequest`的回应 */
    DapCommand,      /** 无线转发的 DAP 命令载荷（USB 收到的 CMSIS-DAP 请求原样打包） */
};

union PacketPayload {
    payload::Probe probe;
    payload::PairingRequest pairing_request;
    payload::PairingAccepted pairing_accepted;
    payload::DapCommand dap_command;
};

class Packet
{
  public:
    Packet();

    esp_err_t deserialize(const u8 *data, size_t len);
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的帧长度，用于发送时告知 ESP-NOW */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;
};

/** 一帧已接收的协议包，附带 ESP-NOW 源 MAC */
struct ReceivedPacket
{
    Packet packet;
    u8 src_mac[ESP_NOW_ETH_ALEN];
};

/** 向指定对端（可为广播地址）发送一帧协议包 */
esp_err_t wireless_link_send(const u8 *dest_mac, const Packet &packet);

/** 打包一条 DAP 命令（原样字节流）为 DapCommand 帧并发送给对端 */
esp_err_t wireless_link_send_dap_command(const u8 *dest_mac, const u8 *data, size_t len);

/**
 * 登记一个对端 peer（encrypt=false）。
 * 仅登记尚未存在于 peer list 的地址，重复调用是安全的。
 * 广播地址已由 wireless_link_init() 常驻登记，无需也不应重复登记。
 */
esp_err_t wireless_link_peer_add(const u8 *peer_mac);

/** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
esp_err_t wireless_link_peer_remove(const u8 *peer_mac);
} // namespace mlink

#endif
