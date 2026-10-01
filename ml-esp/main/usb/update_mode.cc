#include "meguminlink/usb/update_mode.hpp"

#include "meguminlink/app.hpp"
#include "meguminlink/usb/usb_descriptors.hpp"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <tinyusb.h>
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

/** 第 phase 帧的灯效目标：调色板线性插值循环（调色板三段都在红色通道拉满） */
Ws2812LedTarget led_target(u32 phase)
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
    ESP_LOGI(TAG, "Update mode: one vendor interface, LED gradient (hard reset to leave)");

    // 能进入本模式说明这一版固件至少跑得起 USB：确认它可用，取消引导程序的
    // 下一次回滚（见 meguminlink/app.hpp）。回滚确认必须早于任何 esp_ota_begin：
    // 待校验状态下的镜像会被它直接拒绝。
    ota_confirm_running_app();

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

    u32 phase = 0;
    while (true)
    {
        apply_led_frame(phase);
        phase += 1;
        vTaskDelay(pdMS_TO_TICKS(LED_FRAME_MS));
    }
}

bool UpdateMode::usb_control(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    (void)rhport;
    (void)stage;
    // 维护通道本轮没有厂商协议：不接管该请求，由协议栈对 EP0 发 STALL
    ESP_LOGD(TAG, "Vendor control 0x%02X ignored in update mode", request->bRequest);
    return false;
}

const u8 *UpdateMode::usb_bos()
{
    // 更新模式不宣告平台能力：device descriptor 也只报 USB 2.0
    return nullptr;
}

void UpdateMode::usb_rx(u8 idx)
{
    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：回调不携带数据，必须读空内部
    // FIFO，否则主机后续写入会被流控挡住。本轮没有协议，读掉的数据只记字节数，
    // 便于确认主机侧写入确实到达。
    u8 tmp[64];
    u32 total = 0;
    u32 n = 0;
    while ((n = tud_vendor_n_read(idx, tmp, sizeof(tmp))) > 0)
    {
        total += n;
    }
    if (total > 0)
    {
        ESP_LOGD(TAG, "Update-mode bulk RX: %u bytes discarded", static_cast<unsigned>(total));
    }
}

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

    // 配置描述符 = 配置头 + 一个 vendor specific 接口（class 0xFF + 一对 bulk 端点），
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

    constexpr u8 VENDOR_INTERFACE[]{
        TUD_VENDOR_DESCRIPTOR(0, static_cast<u8>(UpdateStringIndex::InterfaceName), UPDATE_EP_OUT, UPDATE_EP_IN, 64)};
    static_assert(sizeof(HEADER) + sizeof(VENDOR_INTERFACE) <= UPDATE_CONFIG_DESCRIPTOR_LEN);

    std::memcpy(m_config, &HEADER, sizeof(HEADER));
    std::memcpy(m_config + sizeof(HEADER), VENDOR_INTERFACE, sizeof(VENDOR_INTERFACE));
    const auto total_length = static_cast<u16>(sizeof(HEADER) + sizeof(VENDOR_INTERFACE));
    // ESP 目标为小端，与 USB 描述符一致
    std::memcpy(m_config + offsetof(tusb_desc_configuration_t, wTotalLength), &total_length, sizeof(total_length));
    m_config[offsetof(tusb_desc_configuration_t, bNumInterfaces)] = 1;

    // 只安装 usb 描述符：不装 CDC、不建 DAP 任务（更新模式不做 CMSIS-DAP）
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

    m_usb_installed = true;
    ESP_LOGI(TAG, "Update-mode USB installed: VID:PID %04X:%04X, 1 vendor interface", USB_VID, USB_PID);
    return ESP_OK;
}

void UpdateMode::apply_led_frame(u32 phase)
{
#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    // 逐帧下发静止色：渐变节奏由 run 的循环掌握，驱动只需最基础的 On 效果
    m_led.apply(led_target(phase));
#else
    (void)phase;
#endif
}
