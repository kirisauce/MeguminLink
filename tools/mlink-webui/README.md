# mlink-webui

MeguminLink 设备监控上位机。浏览器通过 WebUSB 直接读设备的 EP0 厂商统计通道，显示链路状态、各链路实时速率与 60 秒滚动曲线。**无后端、无常驻进程**，页面本身就是一个静态站点。

命令行对照工具是 `tools/mlinkctl.py`，两者解析同一套帧格式、字段名一致。

## 运行

```bash
bun install
bun run dev        # 开发
```

日常使用静态产物：

```bash
bun run build      # 产物在 dist/
bun run preview
```

打开命令输出的地址即可。

## 使用

1. 用 Chrome 或 Edge 打开上面的地址（其它浏览器没有 WebUSB，页面会给出提示）
2. 点右上角「连接设备」，在选择器里选 `MeguminLink CMSIS-DAP on esp32s3`（`303a:4020`）
3. 面板显示链路模式 / 配对状态 / RSSI / 开机时长、各链路速率与累计值、以及两张曲线

页面刷新后会自动接回本 origin 已授权过的设备，不必重新点选。

**监控与烧录可以并存**：页面只发送 EP0 的 device recipient 控制传输，不 claim 任何接口，所以 OpenOCD / pyOCD 可以同时使用同一台设备。

## 设备配置

「配置」按钮（连接后出现在右上角）打开右侧抽屉。

上半是设备身份与配置项：固件版本、芯片、ESP-IDF 版本、当前链路模式、配对状态、已配对对端 MAC。这些字段来自配置通道的 `GET_CONFIG`（cmd 0x02），打开抽屉时刷新一次——版本与配对是低频信息，不占用 200ms 轮询；设备身份读不到时（老固件没有该命令）显示「—」。

下半是操作区：**链路模式**用分段控件切换 local / wireless（当前模式高亮），另有**触发重新配对**（仅 wireless 可用）、**清除配对记录**、**重启设备**、**恢复默认**。三个破坏性动作在按钮上就地二次确认后才下发。

命令返回只代表设备已接受投递，真正的执行在设备主循环（模式与配对状态由 200ms 轮询跟上）；失败（设备忙 / 固件不支持 / 传输错误）在操作区行内提示，命令在途时全部按钮禁用。对应命令行工具是 `mlinkctl set-mode` / `pair` / `unpair` / `reset` / `reboot`。

## 显示约定

- 轮询间隔 200ms；页面切到后台降为 1s。曲线窗口固定 60 秒。
- 速率 = 计数器增量 ÷ **设备自身 uptime** 的增量（不是主机墙钟），单位自适应 B/s、KiB/s、MiB/s；曲线纵轴固定 B/s，横轴是相对当前时刻的秒数。
- 数据不连续时（设备重启、断开重连、页面长时间挂起）曲线在该处断开，不会把两侧连成一条假直线。
- 曲线两张：USB-DAP 一张常驻；另一张跟随链路模式 —— `local` 显示 CDC ↔ 片上 UART，`wireless` 显示 CDC ↔ 无线桥与空口共四条线。

## Linux 权限

如果设备选择器里看不到设备，加一条 udev 规则后重新插拔：

```
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", TAG+="uaccess"
```

## 代码结构

```
src/lib/protocol.ts        EP0 请求构造与 8B 帧头 + TLV 解析（与固件 main/config_channel.cc 契约一致）
src/lib/session.svelte.ts  WebUSB 会话：授权、轮询、拔插、可见性降频、60 秒采样窗口
src/lib/rate.ts            速率差分、u32 回绕、数值与时长格式化
src/styles/tokens.css      M3 token（Megumin 红主色、明暗两套、图表序列色）与元素级基础样式
src/components/            DeviceBar / StatusStrip / StatCard / StatTable / RateChart / Dashboard / ConfigDrawer + ConfigActions
src/pages/index.astro      唯一页面，挂载 Dashboard island
```

## 依赖

`astro`、`@astrojs/svelte` + `svelte`（island）、`uplot`（曲线）、`@types/w3c-web-usb`（TypeScript 的 `lib.dom.d.ts` 不含 WebUSB 类型）。
