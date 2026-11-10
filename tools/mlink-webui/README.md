# mlink-webui

MeguminLink 设备监控上位机。浏览器通过 WebUSB 直接读设备的 EP0 厂商统计通道，显示链路状态、各链路实时速率与 60 秒滚动曲线。**无后端、无常驻进程**，页面本身就是一个静态站点。

命令行对照工具是 `tools/mlinkctl.py`，两者解析同一套帧格式、字段名一致。

## 运行

```bash
pnpm install
pnpm dev           # 开发
```

日常使用静态产物：

```bash
pnpm build         # 产物在 dist/
pnpm preview
```

打开命令输出的地址即可。

## 使用

1. 用 Chrome 或 Edge 打开上面的地址（其它浏览器没有 WebUSB，页面会给出提示）
2. 点右上角「连接设备」，在选择器里选 `MeguminLink CMSIS-DAP on esp32s3`（`303a:4020`）
3. 面板显示链路模式 / 配对状态 / RSSI / 开机时长、各链路速率与累计值、以及两张曲线

顶栏「设备」显示设过的设备名称，没设过就显示设备序列号（MAC）；悬停能看到 USB 产品名与序列号。

本 origin 授权过多台 MeguminLink 时，点顶栏那个设备按钮（还没选设备时它写着「选择设备 ▾」）在下拉里选一台，同一时刻只监控一台（切走的那台会松开句柄、曲线重新开始）；菜单底部的「连接其它设备…」是浏览器设备选择器，用于授权新设备。

选中的那台会被记住，页面刷新、拔插、设备重新枚举、设备重启都自动接回**同一台**，不再抓授权列表里的第一台。记住的是设备序列号（MAC），只存在浏览器本地。

**设备暂时不可用不会中断会话**：被烧录器之类的程序占着（`AccessDeniedError`）、传输被打断、或者设备刚拔掉，都会进入「等待设备」态并按 1s → 2s → 5s 封顶的节奏自动重试，占用解除或插回后自动接回，不需要手动重连。只有浏览器要求用户手势（`SecurityError`）才会停下并提示点「连接设备」。点顶栏「断开」是明确不要了：停止重试，也不再记住这台。

**监控与烧录可以并存**：页面只发送 EP0 的 device recipient 控制传输，不 claim 任何接口，所以 OpenOCD / pyOCD 可以同时使用同一台设备。

## 设备配置

「配置」按钮（连接后出现在右上角）打开右侧抽屉。

上半是设备身份与配置项：固件版本、芯片、ESP-IDF 版本、当前链路模式、配对状态、已配对对端 MAC。这些字段来自配置通道的 `GET_CONFIG`（cmd 0x02），打开抽屉时刷新一次——版本与配对是低频信息，不占用 200ms 轮询；设备身份读不到时（老固件没有该命令）显示「—」。

下半是操作区：**链路模式**用分段控件切换 local / wireless（当前模式高亮），另有**触发重新配对**（仅 wireless 可用）、**清除配对记录**、**重启设备**、**恢复出厂设置**、**进入更新模式**。最后一项只让设备重启进更新模式（USB 只剩一个升级串口），页面随即挂起并提示「设备已进入更新模式，请使用「操作 → 烧录固件…」或手动重连设备」；要回正常模式按复位键或重新插拔一次。

命令返回只代表设备已接受投递，真正的执行在设备主循环（模式与配对状态由 200ms 轮询跟上）；失败（设备忙 / 固件不支持 / 传输错误）在操作区行内提示，命令在途时全部禁用。对应命令行工具是 `mlinkctl set-mode` / `pair` / `unpair` / `reset` / `reboot` / `exit-pair` / `update`。

### USB 接口

配置抽屉中段是 **USB 接口**，对应配置通道的 `GET_USB_CONFIG`（cmd 0x03）/ `SET_USB_CONFIG`（0x16）/ `APPLY_USB_CONFIG`（0x17）：

- **WebUSB 能力**：是否在 BOS 描述符里宣告 WebUSB 平台能力，并提供 landing page 提示
- **WinUSB 自动识别驱动**：是否宣告 Microsoft OS 2.0 能力，让 Windows 自动给 CMSIS-DAP 接口绑定 winusb.sys（免装驱动）
- **WebUSB landing page**：BOS 里 `iLandingPage` 与 `GET_URL` 响应的 URL，最多 126 字符、只能是可打印 ASCII；留空表示回退固件里的默认值
- **HID 兼容模式**：是否在配置描述符里插入 CMSIS-DAP v1 的 HID 接口，默认停用。只认 CMSIS-DAP v1 的调试器需要它，比如部分版本的 Keil；打开后设备会多出一个 HID 接口
- **设备名称**：最多 20 个可打印 ASCII 字符，会附加到 USB 产品名与 DAP_Info 产品名上 —— 显示成 `<名称>(CMSIS-DAP)` 与 `<名称>(MeguminLink CMSIS-DAP)`，括号里的 CMSIS-DAP 关键字始终保留，所以调试器的自动识别不受影响；留空表示不用自定义名，产品名回到 `CMSIS-DAP on <芯片>`。改名会重新枚举 USB

这些项是 **草稿式** 编辑：点「保存并重新枚举」后设备先接收并校验草稿，页面再读一次校验结果，通过才让设备落盘生效。生效需要重新枚举 USB（BOS 与配置描述符只在枚举时被主机读取），所以设备会软断开再连回来，页面在此期间显示「设备正在重新枚举 USB」并自动接回，正在进行的 OpenOCD / pyOCD 调试会断开。

WebUSB / WinUSB 两项能力的**编译期总开关**在固件的 `MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE` / `MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE`，HID 兼容模式的总开关是 `MEGUMINLINK_USB_HID_DAP_ENABLE`（决定 CMSIS-DAP v1 传输路径是否编入）：没编译进来的能力在界面上置灰，设备端也会拒绝启用。Windows 上把 WinUSB 关掉后，已经绑定过 winusb.sys 的设备实例要「卸载设备」再重新插拔才会彻底解除绑定。

对应命令行工具是 `mlinkctl usb`（读）与 `mlinkctl set-usb [--webusb on|off] [--winusb on|off] [--hid-compat on|off] [--landing-page URL] [--name NAME]`；`--hid-compat` 与 `--name` 不带时设备沿用当前值。

## 烧录固件

顶栏 **操作 ▾ → 烧录固件…**：选一个 app 镜像（板子对应的 `build-s3-*/megumin-link-esp.bin`），再点「开始烧录」。

连着设备时，页面先经 WebUSB 让它软复位进更新模式；**页面没连着设备就直接去取串口** —— 设备已经停在更新模式里时走的就是这条路（刷失败之后它不会自己回来）。

1. 取串口：授权过的口逐个握一次手，接不上话的直接跳过 —— 授权列表里可能记着另一块板子，它的口一样是 `303a:4020`
2. 一个都没认下才弹选择器，选 `MeguminLink Update` 那个口；授权过一次就不再弹
3. 写入、校验、切换启动分区，设备自己重启回正常模式，页面自动接回

烧录期间页面**不碰设备**：更新模式对 vendor 请求只有 STALL，让 200ms 轮询继续跑只会刷出一串假故障，所以这段时间会话整个挂起，顶栏停在「等待设备」。烧完等设备回来读一次版本号，跟镜像里写的那份对上才给绿字；对不上多半不是刷错了，而是新固件没起来、被回滚确认带回了旧固件。

选择器弹早了里面会是空的（Web Serial 只列已授权的口，探测不到新口什么时候出现），所以取串口之前会等一段落定时间；真没选到就点「重新选串口」，只重来这一步，不会再把设备重启一遍。

**只刷 app 镜像**。合并镜像、分区表、bootloader 都刷不了：镜像自身的魔数、app 描述结构与尾部 SHA-256 都在发第一个字节之前先验一遍。

连着设备烧时，设备自报的 MAC 会跟当前这台核对，对不上就换下一个口试，全都对不上才停住、一个字节都不写；没连着设备时没有比对基准，页面会把设备自报的 MAC 显示出来让你自己认。老固件不上报 MAC 只提示，不拦。

更新模式只保留一个 CDC 串口，所以数据面走 Web Serial（`navigator.serial`），进更新模式那一步仍走 WebUSB 的 EP0 厂商请求 —— CDC 接口被系统串口驱动占着，WebUSB 打不开它。需要 Chrome 或 Edge；离线单文件版本同样可以烧。

命令行对照是 `mlinkctl flash <file>`：同样的两步，只是用 `/dev/serial/by-id` 自己找口，不需要选择器。

## 显示约定

- 轮询间隔 200ms；页面切到后台降为 1s。曲线窗口固定 60 秒。
- 速率 = 计数器增量 ÷ **设备自身 uptime** 的增量（不是主机墙钟），单位自适应 B/s、KiB/s、MiB/s；曲线纵轴固定 B/s，横轴是相对当前时刻的秒数。
- 数据不连续时（设备重启、断开重连、页面长时间挂起）曲线在该处断开，不会把两侧连成一条假直线。
- 曲线两张：USB-DAP 一张常驻；另一张跟随链路模式 —— `local` 显示 CDC ↔ 片上 UART，`wireless` 显示 CDC ↔ 无线桥与空口共四条线。
- 状态条的**串口**是 CDC ACM 的打开状态（TinyUSB 的 DTR，主机打开 `/dev/ttyACMx` 时置位），由固件在 `GET_STATS` 里实时查询而非缓存，拔插后不会停在过期的「已打开」；**UART 卡片副标题**里的线编码（如 `115200 8N1`）是主机 `SET_LINE_CODING` 下发、固件记录的值。老固件不上报这两项时显示「—」。
- 等待接回期间的提示条用提示色而不是报错色：那是可自愈的中间态；真正需要用户动手的失败（`SecurityError`）才落报错色并停住。

## Linux 权限

如果设备选择器里看不到设备，加一条 udev 规则后重新插拔：

```
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", TAG+="uaccess"
```

## 代码结构

```
src/lib/protocol.ts           EP0 请求构造与 8B 帧头 + TLV 解析（与固件 main/config_channel.cc 契约一致）
src/lib/devices.ts            授权设备条目与「没有选择时该接哪台」的取舍（序列号作身份，纯逻辑）
src/lib/session_recovery.ts   失败归因（占用 / 驱动 / 设备不在 / 要用户动手）与 1s→2s→5s 退避节奏（纯逻辑）
src/lib/session.svelte.ts     WebUSB 会话：授权、轮询、设备切换、按序列号接回、可见性降频、60 秒采样窗口
src/lib/rate.ts               速率差分、u32 回绕、数值与时长格式化
src/lib/update.ts             更新模式串口上的升级协议：帧编解码、停止等待通道、会话握手、镜像本地预检
src/lib/update_port.ts        烧录通道的获取：认出更新模式的设备、握手确认身份、拿到它的 CDC 串口（碰 navigator）
src/styles/tokens.css         M3 token（Megumin 红主色、明暗两套、图表序列色）与元素级基础样式
src/components/ui/            通用组件（SegmentedControl / ConfirmDialog / PopoverMenu），不含设备协议知识，可整块复用
src/components/mlink/shell/   页面骨架与顶栏（Dashboard / DeviceBar / DeviceMenu / FirmwareUpdateDialog）
src/components/mlink/panel/   主面板区，Dashboard 直接渲染（StatusStrip / StatCard / StatTable / RateChart）
src/components/mlink/drawer/  配置抽屉区（ConfigDrawer / ConfigActions / UsbSettings）
src/pages/index.astro         唯一页面，挂载 Dashboard island
```

## 依赖

`astro`、`@astrojs/svelte` + `svelte`（island）、`uplot`（曲线）、`@types/w3c-web-usb`（TypeScript 的 `lib.dom.d.ts` 不含 WebUSB 类型）、`@types/w3c-web-serial`（同理不含 Web Serial）。
