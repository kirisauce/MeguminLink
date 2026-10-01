#include "meguminlink/usb/firmware_updater.hpp"

#include "meguminlink/binfmt.hpp"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cstring>

using namespace mlink;

static constexpr char TAG[] = "mlink::updater";

// ---- 协议（固件与上位机的唯一契约，上位机见 tools/mlinkctl.py 的 flash 子命令）----
//
// 传输：更新模式 vendor 接口的 bulk 端点（OUT 0x02 / IN 0x83），严格停等——主机发
// 一帧、等一帧响应，读到响应才发下一帧。请求与响应同构。
//
// 帧（8B 头 + 载荷，整数一律大端）：
//   magic u16（0x4D4F，字节流恒为 'M' 'O'）+ type u8（命令码，响应回填）+ status u8
//   （响应 = 状态码，请求恒为 0）+ length u16（载荷字节数）+ seq u16（主机生成，
//   响应原样回填），后接 length 字节载荷。
//
// 命令表：
//   0x01 Info    请求空；响应 = 槽位大小 u32 | 分块上限 u32 | 当前固件版本 32B
//   0x02 Begin   请求 = 镜像大小 u32 | 整镜像 SHA-256 32B；响应 = 槽位大小 u32 |
//                分块上限 u32。申报大小超过槽位直接拒绝，一个字节都不擦
//   0x03 Data    请求 = 一段镜像数据（≤ 分块上限）；响应 = 累计已写入 u32
//   0x04 Finish  请求空；响应 = 镜像大小 u32。摘要比对与 esp_ota_end 两级校验都
//                通过才切换启动分区，随后设备自行软复位进新固件
//   0x05 Abort   请求空；响应空。放弃会话，旧固件照常运行
//
// 两级校验：设备对收到的字节流做增量 SHA-256，与 Begin 申报的摘要比对（传输完整
// 性，捕获丢包与翻转）；esp_ota_end 再读回 flash 校验镜像格式与镜像尾部附加的
// SHA-256（写入完整性）。任一级失败都不切换启动分区。
//
// 会话失败后主机重新 Begin 即可重试：新会话先中止旧状态，非启动槽里的半成品随后
// 被新会话的擦除覆盖。

void FirmwareUpdater::handle(const u8 *frame, usize length)
{
    m_last_activity_us = esp_timer_get_time();

    BinDeserializer in(frame, length);
    const u16 magic = in.read_u16();
    const u8 type = in.read_u8();
    in.read_u8(); // status：请求侧恒为 0
    const u16 payload_len = in.read_u16();
    const u16 seq = in.read_u16();

    // 成帧由 UpdateMode 的 RX 解析保证；长度对不上说明调用方传错了帧
    if (magic != UPDATE_FRAME_MAGIC || length != UPDATE_FRAME_HEADER_LEN + payload_len)
    {
        ESP_LOGW(TAG, "Malformed frame: magic 0x%04X, %u bytes", magic, static_cast<unsigned>(length));
        respond(static_cast<UpdateCommand>(type), UpdateStatus::Malformed, seq, nullptr, 0);
        return;
    }

    const u8 *payload = frame + UPDATE_FRAME_HEADER_LEN;
    switch (static_cast<UpdateCommand>(type))
    {
    case UpdateCommand::Info:
        handle_info(seq);
        break;
    case UpdateCommand::Begin:
        handle_begin(payload, payload_len, seq);
        break;
    case UpdateCommand::Data:
        handle_data(payload, payload_len, seq);
        break;
    case UpdateCommand::Finish:
        handle_finish(seq);
        break;
    case UpdateCommand::Abort:
        abort_session();
        respond(UpdateCommand::Abort, UpdateStatus::Ok, seq, nullptr, 0);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported command 0x%02X", type);
        respond(static_cast<UpdateCommand>(type), UpdateStatus::Unsupported, seq, nullptr, 0);
        break;
    }
}

void FirmwareUpdater::handle_info(u16 seq)
{
    // 预检只读分区表，不碰 flash：主机据此判断镜像塞不塞得下、按多大分块发
    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);

    // 版本串来自镜像头，恰好占满字段时不带结尾 NUL
    u8 version[UPDATE_VERSION_LEN] = {};
    std::memcpy(version, esp_app_get_description()->version, sizeof(version) - 1);

    u8 payload[2 * sizeof(u32) + UPDATE_VERSION_LEN] = {};
    BinSerializer out(payload, sizeof(payload));
    out.write_u32((partition != nullptr) ? partition->size : 0);
    out.write_u32(UPDATE_CHUNK_MAX);
    out.write_bytes(version, sizeof(version));

    respond(UpdateCommand::Info, UpdateStatus::Ok, seq, payload, sizeof(payload));
}

void FirmwareUpdater::handle_begin(const u8 *payload, usize payload_len, u16 seq)
{
    if (payload_len != sizeof(u32) + UPDATE_DIGEST_LEN)
    {
        respond(UpdateCommand::Begin, UpdateStatus::Malformed, seq, nullptr, 0);
        return;
    }

    BinDeserializer in(payload, payload_len);
    const u32 image_size = in.read_u32();
    u8 digest[UPDATE_DIGEST_LEN] = {};
    in.read_bytes(digest, sizeof(digest));

    // 主机重试时会带着上一次的残留状态进来：先中止旧的
    abort_session();

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (partition == nullptr)
    {
        ESP_LOGE(TAG, "No update partition available");
        fail(UpdateCommand::Begin, UpdateStatus::NoPartition, seq);
        return;
    }
    if (image_size == 0 || image_size > partition->size)
    {
        ESP_LOGW(TAG, "Image size %u exceeds partition %s (%u bytes)", image_size, partition->label,
                 static_cast<unsigned>(partition->size));
        fail(UpdateCommand::Begin, UpdateStatus::SizeExceeded, seq);
        return;
    }

    esp_ota_handle_t handle = 0;
    // 申报大小让 esp_ota_begin 只擦这个范围，不必整片擦
    const esp_err_t begin_ret = esp_ota_begin(partition, image_size, &handle);
    if (begin_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed on %s: %s", partition->label, esp_err_to_name(begin_ret));
        fail(UpdateCommand::Begin, UpdateStatus::Internal, seq);
        return;
    }

    const psa_status_t hash_ret = psa_hash_setup(&m_hash, PSA_ALG_SHA_256);
    if (hash_ret != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_hash_setup failed: %d", static_cast<int>(hash_ret));
        esp_ota_abort(handle);
        fail(UpdateCommand::Begin, UpdateStatus::Internal, seq);
        return;
    }

    m_partition = partition;
    m_handle = handle;
    m_hash_active = true;
    m_image_size = image_size;
    m_written = 0;
    std::memcpy(m_expected, digest, sizeof(m_expected));
    m_phase = Phase::Receiving;

    ESP_LOGI(TAG, "Receiving %u bytes into %s", image_size, partition->label);

    u8 reply[2 * sizeof(u32)] = {};
    BinSerializer out(reply, sizeof(reply));
    out.write_u32(partition->size);
    out.write_u32(UPDATE_CHUNK_MAX);
    respond(UpdateCommand::Begin, UpdateStatus::Ok, seq, reply, sizeof(reply));
}

void FirmwareUpdater::handle_data(const u8 *payload, usize payload_len, u16 seq)
{
    if (m_phase != Phase::Receiving || m_handle == 0)
    {
        respond(UpdateCommand::Data, UpdateStatus::Sequence, seq, nullptr, 0);
        return;
    }
    if (payload_len == 0 || payload_len > UPDATE_CHUNK_MAX || m_written + payload_len > m_image_size)
    {
        fail(UpdateCommand::Data, UpdateStatus::SizeExceeded, seq);
        return;
    }

    const esp_err_t write_ret = esp_ota_write(m_handle, payload, payload_len);
    if (write_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed at offset %u: %s", m_written, esp_err_to_name(write_ret));
        fail(UpdateCommand::Data, UpdateStatus::FlashError, seq);
        return;
    }

    const psa_status_t hash_ret = psa_hash_update(&m_hash, payload, payload_len);
    if (hash_ret != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_hash_update failed: %d", static_cast<int>(hash_ret));
        fail(UpdateCommand::Data, UpdateStatus::Internal, seq);
        return;
    }

    m_written += static_cast<u32>(payload_len);

    u8 reply[sizeof(u32)] = {};
    BinSerializer out(reply, sizeof(reply));
    out.write_u32(m_written);
    respond(UpdateCommand::Data, UpdateStatus::Ok, seq, reply, sizeof(reply));
}

void FirmwareUpdater::handle_finish(u16 seq)
{
    if (m_phase != Phase::Receiving || m_handle == 0)
    {
        respond(UpdateCommand::Finish, UpdateStatus::Sequence, seq, nullptr, 0);
        return;
    }
    if (m_written != m_image_size)
    {
        ESP_LOGW(TAG, "Short image: %u of %u bytes received", m_written, m_image_size);
        fail(UpdateCommand::Finish, UpdateStatus::SizeExceeded, seq);
        return;
    }

    u8 digest[UPDATE_DIGEST_LEN] = {};
    usize digest_len = 0;
    const psa_status_t hash_ret = psa_hash_finish(&m_hash, digest, sizeof(digest), &digest_len);
    m_hash_active = false;
    if (hash_ret != PSA_SUCCESS || digest_len != sizeof(digest))
    {
        ESP_LOGE(TAG, "psa_hash_finish failed: %d", static_cast<int>(hash_ret));
        fail(UpdateCommand::Finish, UpdateStatus::Internal, seq);
        return;
    }
    if (std::memcmp(digest, m_expected, sizeof(digest)) != 0)
    {
        ESP_LOGE(TAG, "Digest mismatch: image does not match the announced SHA-256");
        fail(UpdateCommand::Finish, UpdateStatus::HashMismatch, seq);
        return;
    }

    // 第二级校验：esp_ota_end 读回 flash，校验镜像格式与镜像尾部的 SHA-256
    const esp_err_t end_ret = esp_ota_end(m_handle);
    m_handle = 0; // 成功与否句柄都已释放
    if (end_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end rejected the image: %s", esp_err_to_name(end_ret));
        const UpdateStatus status =
            (end_ret == ESP_ERR_OTA_VALIDATE_FAILED) ? UpdateStatus::ImageInvalid : UpdateStatus::Internal;
        fail(UpdateCommand::Finish, status, seq);
        return;
    }

    const esp_err_t boot_ret = esp_ota_set_boot_partition(m_partition);
    if (boot_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(boot_ret));
        fail(UpdateCommand::Finish, UpdateStatus::Internal, seq);
        return;
    }

    ESP_LOGI(TAG, "Image verified (%u bytes), next boot from %s", m_image_size, m_partition->label);

    m_phase = Phase::Done;

    u8 reply[sizeof(u32)] = {};
    BinSerializer out(reply, sizeof(reply));
    out.write_u32(m_image_size);
    respond(UpdateCommand::Finish, UpdateStatus::Ok, seq, reply, sizeof(reply));
}

void FirmwareUpdater::poll()
{
    const i64 now = esp_timer_get_time();
    switch (m_phase)
    {
    case Phase::Receiving:
        if (static_cast<u32>((now - m_last_activity_us) / 1000) >= UPDATE_SESSION_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "Session idle for %u ms at %u/%u bytes, aborted",
                     static_cast<unsigned>(UPDATE_SESSION_TIMEOUT_MS), m_written, m_image_size);
            abort_session();
        }
        break;
    case Phase::Failed:
        if (static_cast<u32>((now - m_failed_at_us) / 1000) >= UPDATE_FAILED_HOLD_MS)
        {
            m_phase = Phase::Idle;
        }
        break;
    default:
        break;
    }
}

void FirmwareUpdater::abort_session()
{
    if (m_hash_active)
    {
        psa_hash_abort(&m_hash);
        m_hash_active = false;
    }
    if (m_handle != 0)
    {
        esp_ota_abort(m_handle);
        m_handle = 0;
    }

    m_partition = nullptr;
    m_image_size = 0;
    m_written = 0;

    // 已切了启动分区的会话不再回失败态：镜像马上就要被引导起来
    if (m_phase != Phase::Done)
    {
        m_phase = Phase::Failed;
        m_failed_at_us = esp_timer_get_time();
    }
}

u8 FirmwareUpdater::progress_percent() const
{
    if (m_image_size == 0)
    {
        return 0;
    }
    return static_cast<u8>(static_cast<u64>(m_written) * 100 / m_image_size);
}

void FirmwareUpdater::respond(UpdateCommand command, UpdateStatus status, u16 seq, const u8 *payload, usize payload_len)
{
    BinSerializer out(m_response, sizeof(m_response));
    out.write_u16(UPDATE_FRAME_MAGIC);
    out.write_u8(static_cast<u8>(command));
    out.write_u8(static_cast<u8>(status));
    out.write_u16(static_cast<u16>(payload_len));
    out.write_u16(seq);
    if (payload_len > 0)
    {
        out.write_bytes(payload, payload_len);
    }
    m_response_length = out.size();
}

void FirmwareUpdater::fail(UpdateCommand command, UpdateStatus status, u16 seq)
{
    respond(command, status, seq, nullptr, 0);
    abort_session();
}
