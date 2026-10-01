#pragma once
#ifndef __MEGUMINLINK_LED_TYPES_HPP__
#define __MEGUMINLINK_LED_TYPES_HPP__

#include "esp_err.h"
#include "meguminlink/common.hpp"

#include <concepts>

namespace mlink
{
/** RGB888 颜色：各驱动描述"要点亮什么颜色"的公共词汇 */
struct LedColor
{
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
};

/**
 * 设备灯效场景：上层只报告"设备现在处于什么状态"，至于该状态用什么颜色、常亮
 * 还是闪烁、要不要渐变，全部由各驱动自行决定（表现层归驱动，上层不认识任何颜色）。
 *
 * 正常模式与更新模式共用这一套场景：更新模式不构造 Application，两者不会同时存在，
 * 所以一个场景枚举足以描述整台设备的所有指示灯状态。
 */
enum class LedScene : u8
{
    Local,           // 本地调试（USB CMSIS-DAP）已就绪
    Wireless,        // 无线调试，已配对
    Pairing,         // 配对流程中
    UpdateIdle,      // 更新模式空闲，等待主机接入
    UpdateReceiving, // 更新模式接收固件中（progress 有效）
    UpdateVerifying, // 更新模式校验镜像中（读回整个分区，耗时较长）
    UpdateDone,      // 更新成功，启动分区已切换（随后复位进新固件）
    UpdateFailed,    // 上一次升级会话失败（可重试）
};

/** 一次灯效请求：场景 + 该场景附带的进度 */
struct LedRequest
{
    LedScene scene = LedScene::Local;
    u8 progress = 0; // UpdateReceiving：已接收百分比 0-100；其余场景忽略

    bool operator==(const LedRequest &) const = default;
};

/**
 * LED 驱动约束：驱动必须能初始化，并实现场景渲染。
 *
 * 这是编译期多态的约束面：选型处（led_driver.hpp）对最终选中的类型做
 * static_assert，接口写歪（参数类型不符、返回值不是 esp_err_t）在这里立刻报错，
 * 而不是等到某个调用点冒出一堆模板实例化错误。
 */
template <typename T>
concept LedDriverConcept = requires(T driver, const LedRequest &request) {
    { driver.init() } -> std::same_as<esp_err_t>;
    { driver.apply_impl(request) } -> std::same_as<void>;
};

/**
 * LED 驱动基类（CRTP）：统一下发入口 + 各项能力的默认实现。
 *
 * 对外只有 apply() 一个下发入口（非虚），去重在这里做：请求与上次相同时直接返回，
 * 所以上层可以每拍无条件上报场景，不必自己记住"上次报的是什么"，硬件也不会被
 * 反复重写。具体表现由派生类实现 apply_impl()；没实现的场景保持黑灯，不会把设备
 * 卡在初始化失败上。
 *
 * 用 CRTP 而不是普通基类，是为了堵住"把驱动当基类用"的误用：普通基类下
 * `LedDriverBase &base = driver; base.apply(req);` 会编译通过并静默落到空实现
 * （灯不亮却查不出原因）；CRTP 基类只作为派生类的一部分存在，没有可独立绑定的类型。
 */
template <typename Derived> class LedDriverBase
{
  public:
    /** 默认：不占用任何外设 */
    esp_err_t init()
    {
        return ESP_OK;
    }

    /** 场景下发入口：与上次相同则不下发，否则交给派生类渲染 */
    void apply(const LedRequest &request)
    {
        if (m_applied && request == m_request)
        {
            return;
        }
        m_request = request;
        m_applied = true;
        static_cast<Derived *>(this)->apply_impl(request);
    }

    /** 默认：忽略该场景，保持黑灯（由 apply 调用，驱动不必自己判重） */
    void apply_impl(const LedRequest &)
    {
    }

  private:
    LedRequest m_request{}; /**< 最近下发的请求 */
    bool m_applied = false; /**< 是否已下发过（首次必须下发，而不是当作未变化跳过） */
};

/**
 * 空 LED 驱动：LED 模块整体关闭（MEGUMINLINK_LED_DRIVER_NONE）时选型落在这里，
 * 全部能力都用基类的默认空实现。
 *
 * 有了它，上层（Application、更新模式）不必为"LED 没编进固件"再加一层编译期
 * 判断：场景照常上报，只是没有任何物理输出。
 */
class NullLedDriver : public LedDriverBase<NullLedDriver>
{
};
} // namespace mlink

#endif
