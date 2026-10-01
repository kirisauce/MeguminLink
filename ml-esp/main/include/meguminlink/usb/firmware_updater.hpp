#pragma once
#ifndef __MEGUMINLINK_FIRMWARE_UPDATER_HPP__
#define __MEGUMINLINK_FIRMWARE_UPDATER_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#include <esp_ota_ops.h>
#include <psa/crypto.h>

namespace mlink
{
/** 升级协议帧头的魔数（"MO"）：更新模式 vendor bulk 通道上唯一的成帧标识 */
inline constexpr u16 UPDATE_FRAME_MAGIC = 0x4D4F;

/** 升级协议帧头长度：帧头 + 载荷 = 一帧 */
inline constexpr usize UPDATE_FRAME_HEADER_LEN = 8;

/**
 * 单个 DATA 帧的最大载荷：一个 flash 扇区，与 esp_ota_write 的写入粒度一致。
 * 分块越大，单帧的帧头与往返开销占比越低。
 */
inline constexpr usize UPDATE_CHUNK_MAX = 4096;

/** 单帧最大字节数（帧头 + 最大载荷）：RX 解析缓冲与帧队列元素都按它开 */
inline constexpr usize UPDATE_FRAME_MAX = UPDATE_FRAME_HEADER_LEN + UPDATE_CHUNK_MAX;

/** 镜像 SHA-256 摘要长度 */
inline constexpr usize UPDATE_DIGEST_LEN = 32;

/** 固件版本串长度（esp_app_desc_t::version 的字段宽度） */
inline constexpr usize UPDATE_VERSION_LEN = 32;

/** 响应帧最大字节数：Info 响应 = 槽位大小 + 分块上限 + 版本串 */
inline constexpr usize UPDATE_RESPONSE_MAX = UPDATE_FRAME_HEADER_LEN + 2 * sizeof(u32) + UPDATE_VERSION_LEN;

/** 传输空闲多久没有数据即放弃本次会话（主机掉线/卡死时不把半成品状态一直挂着） */
inline constexpr u32 UPDATE_SESSION_TIMEOUT_MS = 10000;

/** 会话失败后红灯保持多久，之后回到空闲等待下一次 Begin */
inline constexpr u32 UPDATE_FAILED_HOLD_MS = 3000;

/** 升级协议命令码（主机 → 设备；响应帧回填同一码） */
enum class UpdateCommand : u8
{
    Info = 0x01,   // 预检：槽位大小、分块上限、设备当前固件版本
    Begin = 0x02,  // 申报镜像大小与 SHA-256，擦除目标槽并开始接收
    Data = 0x03,   // 一段镜像数据
    Finish = 0x04, // 校验摘要与镜像，通过则切换启动分区
    Abort = 0x05,  // 放弃本次会话
};

/**
 * 升级协议状态码。数值与配置通道的 status 对齐（0x00 ok / 0x02 malformed /
 * 0x03 internal / 0x04 unsupported），0x05 busy 在本通道没有语义，因此跳过。
 */
enum class UpdateStatus : u8
{
    Ok = 0x00,           // 成功
    Malformed = 0x02,    // 帧载荷长度与命令不符
    Internal = 0x03,     // 设备内部错误（esp_ota_* 或摘要运算失败）
    Unsupported = 0x04,  // 未知命令
    NoPartition = 0x06,  // 没有可写的 OTA 槽（分区表里只有一个 app 槽）
    HashMismatch = 0x07, // 收到的字节流与申报的 SHA-256 不符
    ImageInvalid = 0x08, // 镜像本身非法（esp_ota_end 校验失败）
    SizeExceeded = 0x09, // 申报大小越过槽位，或累计大小越过申报值
    Sequence = 0x0A,     // 命令顺序错误（未 Begin 就 Data 等）
    FlashError = 0x0B,   // flash 写入失败
};

/**
 * 更新模式 vendor bulk 通道上的固件升级接收端（协议见 firmware_updater.cc 顶部）。
 *
 * 一个会话把主机推来的 app 镜像写进空闲 OTA 槽：Begin 申报大小与整镜像 SHA-256，
 * Data 分块写入并同步累计摘要，Finish 先比摘要、再由 esp_ota_end 读回 flash 校验
 * 镜像本身，两级都过才切换启动分区。任何一步失败都中止会话并保留当前固件：半成品
 * 只落在非启动槽，复位后仍然是旧固件。
 *
 * 只处理成帧后的请求；成帧与同步（找魔数、按声明长度收齐）由 UpdateMode 的 RX 解析
 * 完成。运行状态全部收在本对象里，实例由 UpdateMode 持有。
 */
class FirmwareUpdater
{
  public:
    /** 会话阶段：灯效与复位时机由它决定 */
    enum class Phase : u8
    {
        Idle,      // 空闲：等待 Begin（灯效渐变）
        Receiving, // 接收中（灯效按进度变色）
        Done,      // 校验通过且已切换启动分区（灯效绿，随后复位）
        Failed,    // 上一次会话失败（灯效红闪，保持 UPDATE_FAILED_HOLD_MS 后回 Idle）
    };

    /** 处理一条请求帧；响应帧由 response() 取用 */
    void handle(const u8 *frame, usize length);

    /** 周期调用：接收空闲超时自动中止，失败状态到期回到空闲 */
    void poll();

    /** 中止会话（主机断开、超时、命令失败共用）；当前固件不受影响 */
    void abort_session();

    Phase phase() const
    {
        return m_phase;
    }

    /** 接收进度 0-100（仅 Receiving 阶段有意义） */
    u8 progress_percent() const;

    /** 最近组好的响应帧（完整帧，含帧头） */
    const u8 *response() const
    {
        return m_response;
    }

    usize response_length() const
    {
        return m_response_length;
    }

  private:
    void handle_info(u16 seq);
    void handle_begin(const u8 *payload, usize payload_len, u16 seq);
    void handle_data(const u8 *payload, usize payload_len, u16 seq);
    void handle_finish(u16 seq);

    /** 组一条响应帧 */
    void respond(UpdateCommand command, UpdateStatus status, u16 seq, const u8 *payload, usize payload_len);

    /** 回错误响应并中止会话（命令处理失败时的统一出口） */
    void fail(UpdateCommand command, UpdateStatus status, u16 seq);

    esp_ota_handle_t m_handle = 0;
    const esp_partition_t *m_partition = nullptr;
    psa_hash_operation_t m_hash = PSA_HASH_OPERATION_INIT;
    bool m_hash_active = false;

    u32 m_image_size = 0;                  /**< 主机申报的镜像字节数 */
    u32 m_written = 0;                     /**< 已写入并计入摘要的字节数 */
    u8 m_expected[UPDATE_DIGEST_LEN] = {}; /**< 主机申报的整镜像 SHA-256 */

    Phase m_phase = Phase::Idle;
    i64 m_last_activity_us = 0; /**< 最近一次收到帧的时刻（poll 判定接收空闲超时） */
    i64 m_failed_at_us = 0;     /**< 最近一次进入失败态的时刻（poll 判定红灯到期） */

    u8 m_response[UPDATE_RESPONSE_MAX] = {};
    usize m_response_length = 0;
};
} // namespace mlink

#endif // __MEGUMINLINK_FIRMWARE_UPDATER_HPP__
