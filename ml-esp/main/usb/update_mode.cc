#include "meguminlink/usb/update_mode.hpp"

#include "meguminlink/app.hpp"
#include "meguminlink/binfmt.hpp"
#include "meguminlink/usb/usb_descriptors.hpp"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_default_config.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace mlink;

static constexpr char TAG[] = "mlink::update";

namespace
{
// ---- 跨软复位的进入请求 ----

/** 进入请求的魔数（"MLSV"）：上电时 .noinit 段内容未初始化，靠它区分真实请求 */
constexpr u32 UPDATE_BOOT_MAGIC = 0x4D4C5356;

/**
 * 进入请求的跨复位载体：唯一被特许的文件级 static —— 它必须在任何对象构造之前
 * 独立存在，并落在 .noinit 段（__NOINIT_ATTR）以保证软件复位后内容保留。上电时
 * 该段是未初始化内容，因此用魔数 + 反码双重校验，避免随机值误判。
 */
struct UpdateBootRequest
{
    u32 magic;
    u32 inverse;
};

__NOINIT_ATTR UpdateBootRequest s_boot_request;

// ---- 最小描述符集 ----

/** 语言 ID（英文），esp_tinyusb 直接拷贝其原始 2 字节 */
constexpr char LANGID[] = {0x09, 0x04};
constexpr char MANUFACTURER_NAME[] = "MeguminLink";
constexpr char PRODUCT_NAME[] = "MeguminLink Update";
constexpr char INTERFACE_NAME[] = "MeguminLink Update";

void usb_event_callback([[maybe_unused]] tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

// ---- 灯效 ----

/** 灯效帧间隔（毫秒）：更新模式循环的节拍（与 WS2812 驱动的动画帧同一粒度） */
constexpr u32 LED_FRAME_MS = 30;

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
/** 一轮渐变的帧数：三段等分，100 帧 × 30ms = 3s 每段，一轮 9s */
constexpr u32 LED_CYCLE_FRAMES = 300;

/** 渐变调色板：红 → 橙 → 粉（末段由粉插值回红） */
constexpr u8 PALETTE[][3] = {
    {255, 0, 0},
    {255, 128, 0},
    {255, 64, 160},
};
constexpr u32 PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);
constexpr u32 SEGMENT_FRAMES = LED_CYCLE_FRAMES / PALETTE_SIZE;

static_assert(LED_CYCLE_FRAMES % PALETTE_SIZE == 0,
              "gradient cycle must split evenly into one segment per palette entry");

/** 整数线性插值：step ∈ [0, span)，结果只落在 from 与 to 之间 */
u8 lerp(u8 from, u8 to, u32 step, u32 span)
{
    const i32 delta = static_cast<i32>(to) - static_cast<i32>(from);
    return static_cast<u8>(static_cast<i32>(from) + delta * static_cast<i32>(step) / static_cast<i32>(span));
}

/** 第 phase 帧的渐变目标色：调色板线性插值循环（调色板三段都在红色通道拉满） */
Ws2812LedTarget gradient_target(u32 phase)
{
    const u32 position = phase % LED_CYCLE_FRAMES;
    const u32 segment = position / SEGMENT_FRAMES;
    const u32 step = position % SEGMENT_FRAMES;
    const u8 *from = PALETTE[segment];
    const u8 *to = PALETTE[(segment + 1) % PALETTE_SIZE];

    return Ws2812LedTarget{lerp(from[0], to[0], step, SEGMENT_FRAMES), lerp(from[1], to[1], step, SEGMENT_FRAMES),
                           lerp(from[2], to[2], step, SEGMENT_FRAMES), Ws2812LedEffect::On};
}
#endif

// ---- 时序 ----

/** 校验通过、启动分区已切换后，留给主机读走最后一条响应再复位的时间 */
constexpr u32 REBOOT_DELAY_MS = 1000;

/** UpdateMode 单例访问点：TinyUSB 回调借它判断是否处于更新模式 */
UpdateMode *&instance_slot()
{
    static UpdateMode *instance = nullptr; // 常量初始化：无竞争、无懒初始化 guard 开销
    return instance;
}
} // namespace

void mlink::update_mode::request()
{
    // 先写反码再写魔数：写入被复位或掉电打断时校验也不会误判
    s_boot_request.inverse = ~UPDATE_BOOT_MAGIC;
    s_boot_request.magic = UPDATE_BOOT_MAGIC;
}

bool mlink::update_mode::consume_request()
{
    const bool requested = s_boot_request.magic == UPDATE_BOOT_MAGIC && s_boot_request.inverse == ~UPDATE_BOOT_MAGIC;

    // 读后立即清除：进入请求是一次性的，任何后续复位都回到正常模式
    s_boot_request.magic = 0;
    s_boot_request.inverse = 0;
    return requested;
}

UpdateMode &UpdateMode::get_instance()
{
    static UpdateMode instance;
    instance_slot() = &instance;
    return instance;
}

UpdateMode *UpdateMode::try_instance()
{
    return instance_slot();
}

void UpdateMode::run()
{
    ESP_LOGI(TAG, "Update mode: firmware update channel on one vendor interface");

    // 能进入本模式说明这一版固件至少跑得起 USB：确认它可用，取消引导程序的
    // 下一次回滚（见 meguminlink/app.hpp）。回滚确认必须早于任何 esp_ota_begin：
    // 待校验状态下的镜像会被它直接拒绝。
    ota_confirm_running_app();

    // 帧队列在 USB 之前建好：RX 回调随时可能投帧
    m_frame_queue = xQueueCreate(UPDATE_FRAME_QUEUE_DEPTH, UPDATE_FRAME_MAX);
    if (m_frame_queue == nullptr)
    {
        ESP_LOGE(TAG, "Frame queue allocation failed, firmware update unavailable");
    }

    // USB 不可用不阻塞模式进入：灯效照常跑，原因看日志
    const esp_err_t usb_ret = init_usb();
    if (usb_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Update-mode USB unavailable: %s", esp_err_to_name(usb_ret));
    }

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    const esp_err_t led_ret = m_led.init();
    if (led_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Update-mode LED unavailable: %s", esp_err_to_name(led_ret));
    }
#endif

    m_led_epoch_us = esp_timer_get_time();
    i64 next_led_us = m_led_epoch_us;

    while (true)
    {
        // 等帧的时长刚好是到下一帧灯效的时刻：有帧立刻醒（写 flash 的间隙也持续
        // 刷灯），没帧就睡到该刷灯的时候，不做空转轮询
        i64 wait_us = next_led_us - esp_timer_get_time();
        if (wait_us < 0)
        {
            wait_us = 0;
        }
        const u32 wait_ms = static_cast<u32>((wait_us + 999) / 1000);

        if (m_frame_queue != nullptr && xQueueReceive(m_frame_queue, m_frame_buf, pdMS_TO_TICKS(wait_ms)) == pdTRUE)
        {
            handle_frame(m_frame_buf);
            continue;
        }

        apply_led_frame();
        m_updater.poll();
        next_led_us = esp_timer_get_time() + LED_FRAME_MS * 1000;
    }
}

bool UpdateMode::usb_control(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    (void)rhport;
    (void)stage;
    // 升级协议走 bulk 端点，EP0 上没有厂商协议：不接管该请求，由协议栈对 EP0 发 STALL
    ESP_LOGD(TAG, "Vendor control 0x%02X ignored in update mode", request->bRequest);
    return false;
}

const u8 *UpdateMode::usb_bos()
{
    // 更新模式不宣告平台能力：device descriptor 也只报 USB 2.0
    return nullptr;
}

void UpdateMode::cdc_rx_callback(int itf, [[maybe_unused]] cdcacm_event_t *event)
{
    if (itf != TINYUSB_CDC_ACM_0)
    {
        return;
    }
    // 本回调只在本模式下注册，那时实例必然已构造
    if (auto *mode = try_instance())
    {
        mode->on_cdc_rx();
    }
}

void UpdateMode::on_cdc_rx()
{
    // 回调不携带数据：必须读空内部 FIFO，否则主机后续写入会被流控挡住。切片成帧
    // 在本任务里做，写 flash 只由主循环做（flash 操作会阻塞任务，不能压在 USB 任务上）。
    for (;;)
    {
        u8 chunk[256];
        size_t rx_size = 0;
        const esp_err_t ret = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, chunk, sizeof(chunk), &rx_size);
        if (ret != ESP_OK || rx_size == 0)
        {
            return;
        }
        feed_rx(chunk, rx_size);
    }
}

void UpdateMode::feed_rx(const u8 *data, usize len)
{
    while (len > 0)
    {
        const usize space = sizeof(m_rx_buf) - m_rx_len;
        const usize take = (len < space) ? len : space;
        if (take == 0)
        {
            // 缓冲已满且切不出帧（声明长度非法）：丢一字节腾地方，避免卡死
            drop_rx(1);
            continue;
        }
        std::memcpy(m_rx_buf + m_rx_len, data, take);
        m_rx_len += take;
        data += take;
        len -= take;

        // 反复从缓冲开头切帧：收齐一条就投一条，收不齐就留着等后续 USB 包
        for (;;)
        {
            if (m_rx_len < UPDATE_FRAME_HEADER_LEN)
            {
                break;
            }

            BinDeserializer in(m_rx_buf, m_rx_len);
            if (in.read_u16() != UPDATE_FRAME_MAGIC)
            {
                drop_rx(1); // 残包或噪声：丢一字节重新找魔数
                continue;
            }

            in.read_u8();
            in.read_u8();
            const usize frame_len = UPDATE_FRAME_HEADER_LEN + in.read_u16();
            if (frame_len > sizeof(m_rx_buf))
            {
                // 这个"帧头"声明的长度超过单帧上限：不是真帧头，继续重同步
                drop_rx(1);
                continue;
            }
            if (m_rx_len < frame_len)
            {
                if (m_rx_len == sizeof(m_rx_buf))
                {
                    drop_rx(1); // 缓冲满了都凑不齐：这段字节里没有合法帧
                    continue;
                }
                break; // 还没收齐
            }

            if (m_frame_queue == nullptr || xQueueSend(m_frame_queue, m_rx_buf, 0) != pdTRUE)
            {
                // 队列满说明主机没守停等：丢弃本条，它等不到响应会自行重试
                ESP_LOGW(TAG, "Frame queue full, %u-byte frame dropped", static_cast<unsigned>(frame_len));
            }
            drop_rx(frame_len);
        }
    }
}

void UpdateMode::drop_rx(usize n)
{
    if (n >= m_rx_len)
    {
        m_rx_len = 0;
        return;
    }
    std::memmove(m_rx_buf, m_rx_buf + n, m_rx_len - n);
    m_rx_len -= n;
}

void UpdateMode::handle_frame(const u8 *frame)
{
    const u8 type = frame[2];
    const usize length = UPDATE_FRAME_HEADER_LEN + (static_cast<usize>(frame[4]) << 8) + frame[5];

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    // Finish 要读回整个分区校验，主任务会在这几百毫秒里阻塞、循环刷不了灯：
    // 先钉住"正在校验"的颜色，让现场一眼看出设备没有卡死
    if (type == static_cast<u8>(UpdateCommand::Finish))
    {
        m_led.apply(Ws2812LedTarget{255, 192, 0, Ws2812LedEffect::On});
    }
#endif

    m_updater.handle(frame, length);
    send_response();

    if (m_updater.phase() == FirmwareUpdater::Phase::Done)
    {
        // 校验通过、启动分区已切换：先给成功色，留出主机读走最后一条响应的
        // 余量，然后进新固件
        apply_led_frame();
        vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
        ESP_LOGI(TAG, "Restarting into the new firmware");
        esp_restart();
    }
}

void UpdateMode::send_response()
{
    const u8 *data = m_updater.response();
    const usize len = m_updater.response_length();
    if (len == 0 || !m_usb_installed)
    {
        return;
    }

    // 写队列同步拷贝进 CDC TX FIFO：响应只有几十字节，远小于 FIFO，停等协议下主机
    // 总会及时取走，因此不等 TX 完成
    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    if (queued < len)
    {
        ESP_LOGW(TAG, "CDC TX queue full, dropped %u of %u bytes", static_cast<unsigned>(len - queued),
                 static_cast<unsigned>(len));
        return;
    }
    // 写入字节数 < 端点包长时不会自动发起 IN 传输，必须显式 flush
    const esp_err_t ret = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "CDC flush failed: %s", esp_err_to_name(ret));
    }
}

void UpdateMode::apply_led_frame()
{
#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    m_led.apply(led_target());
#endif
}

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
Ws2812LedTarget UpdateMode::led_target()
{
    switch (m_updater.phase())
    {
    case FirmwareUpdater::Phase::Receiving: {
        // 蓝 → 青：绿灯分量跟着进度走，一眼看出还剩多少
        const u8 progress = m_updater.progress_percent();
        return Ws2812LedTarget{0, lerp(64, 255, progress, 100), 255, Ws2812LedEffect::On};
    }
    case FirmwareUpdater::Phase::Done:
        // 闪光而不是常亮：复位前只有 REBOOT_DELAY_MS 这一小段，闪烁更容易被看见
        return Ws2812LedTarget{0, 255, 0, Ws2812LedEffect::Blink};
    case FirmwareUpdater::Phase::Failed:
        return Ws2812LedTarget{255, 0, 0, Ws2812LedEffect::Blink};
    case FirmwareUpdater::Phase::Idle:
    default: {
        // 相位跟真实时间走：主循环忙于写 flash 时帧不会挤在一起，9s 一轮不漂
        const i64 elapsed_us = esp_timer_get_time() - m_led_epoch_us;
        return gradient_target(static_cast<u32>(elapsed_us / (LED_FRAME_MS * 1000)));
    }
    }
}
#endif

esp_err_t UpdateMode::init_usb()
{
    // 串号 = 芯片 MAC，与正常模式同一口径：主机据此认出是同一台设备
    u8 mac[6];
    const esp_err_t mac_ret = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (mac_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(mac_ret));
        return mac_ret;
    }
    snprintf(m_serial, sizeof(m_serial), MACSTR, MAC2STR(mac));

    m_string_table[(usize)UpdateStringIndex::LangId] = LANGID;
    m_string_table[(usize)UpdateStringIndex::Manufacturer] = MANUFACTURER_NAME;
    m_string_table[(usize)UpdateStringIndex::Product] = PRODUCT_NAME;
    m_string_table[(usize)UpdateStringIndex::SerialNumber] = m_serial;
    m_string_table[(usize)UpdateStringIndex::InterfaceName] = INTERFACE_NAME;

    m_device = {};
    m_device.bLength = sizeof(tusb_desc_device_t);
    m_device.bDescriptorType = TUSB_DESC_DEVICE;
    m_device.bcdUSB = 0x0200; // USB 2.0：不宣告 BOS 平台能力
    m_device.bDeviceClass = 0x00;
    m_device.bDeviceSubClass = 0x00;
    m_device.bDeviceProtocol = 0x00;
    m_device.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    m_device.idVendor = USB_VID;
    m_device.idProduct = USB_PID;
    m_device.bcdDevice = 0x0100;
    m_device.iManufacturer = static_cast<u8>(UpdateStringIndex::Manufacturer);
    m_device.iProduct = static_cast<u8>(UpdateStringIndex::Product);
    m_device.iSerialNumber = static_cast<u8>(UpdateStringIndex::SerialNumber);
    m_device.bNumConfigurations = 0x01;

    // 配置描述符 = 配置头 + 一个 CDC 接口段（通信接口 + 数据接口），
    // bNumInterfaces 与 wTotalLength 收尾回填
    constexpr tusb_desc_configuration_t HEADER = {
        .bLength = TUD_CONFIG_DESC_LEN,
        .bDescriptorType = TUSB_DESC_CONFIGURATION,
        .wTotalLength = 0,
        .bNumInterfaces = 0,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = 0x80, // bus powered
        .bMaxPower = 500 / 2, // 单位 2 mA
    };
    static_assert(sizeof(HEADER) == TUD_CONFIG_DESC_LEN);

    constexpr u8 CDC_INTERFACE[]{TUD_CDC_DESCRIPTOR(0, static_cast<u8>(UpdateStringIndex::InterfaceName),
                                                    UPDATE_EP_CDC_NOTIF, 64, UPDATE_EP_CDC_OUT, UPDATE_EP_CDC_IN, 64)};
    static_assert(sizeof(HEADER) + sizeof(CDC_INTERFACE) <= UPDATE_CONFIG_DESCRIPTOR_LEN);

    std::memcpy(m_config, &HEADER, sizeof(HEADER));
    std::memcpy(m_config + sizeof(HEADER), CDC_INTERFACE, sizeof(CDC_INTERFACE));
    const auto total_length = static_cast<u16>(sizeof(HEADER) + sizeof(CDC_INTERFACE));
    // ESP 目标为小端，与 USB 描述符一致
    std::memcpy(m_config + offsetof(tusb_desc_configuration_t, wTotalLength), &total_length, sizeof(total_length));
    m_config[offsetof(tusb_desc_configuration_t, bNumInterfaces)] = UPDATE_INTERFACE_COUNT;

    // 只装 CDC：不建 DAP 任务（更新模式不做 CMSIS-DAP），也没有 vendor/HID 接口
    tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG(usb_event_callback);
    tinyusb_config.descriptor.device = &m_device;
    tinyusb_config.descriptor.string = m_string_table.data();
    tinyusb_config.descriptor.string_count = static_cast<int>(m_string_table.size());
    tinyusb_config.descriptor.full_speed_config = m_config;

#if !CONFIG_FREERTOS_UNICORE
    // 与正常模式同一份任务布局：TinyUSB 固定在 core 1
    tinyusb_config.task.xCoreID = 1;
    tinyusb_config.task.priority = 5;
#endif

    const esp_err_t ret = tinyusb_driver_install(&tinyusb_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // CDC 收发回调必须在驱动安装之后注册：CDC 实例随驱动建立
    tinyusb_config_cdcacm_t cdc_config = {};
    cdc_config.callback_rx = cdc_rx_callback;
    const esp_err_t cdc_ret = tinyusb_cdcacm_init(&cdc_config);
    if (cdc_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init failed: %s", esp_err_to_name(cdc_ret));
        return cdc_ret;
    }

    m_usb_installed = true;
    ESP_LOGI(TAG, "Update-mode USB installed: VID:PID %04X:%04X, 1 CDC interface", USB_VID, USB_PID);
    return ESP_OK;
}
