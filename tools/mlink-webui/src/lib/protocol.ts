/**
 * MeguminLink 配置通道的 EP0 厂商控制请求与 TLV 响应解析。
 *
 * 传输：bmRequestType = 0xC0（IN、vendor、device recipient）、bRequest = 0x4D（'M'）、
 * wValue = 命令码、wIndex = seq（设备原样回显）、wLength = 期望响应长度。
 * 只读命令没有 DATA OUT 阶段，命令参数走 wValue，因此请求不带帧头，响应才是完整一帧。
 *
 * 帧头 8B 大端：magic u16（0x4D4C，字节流恒为 'M' 'L'）+ cmd u8 + seq u8
 * + len u16（TLV 区长度）+ status u8 + reserved u8，后接 len 字节 TLV 区。
 * TLV：tag u8 | len u8 | value，len == 0xFF 时后接 u16 大端扩展长度；
 * 整数一律大端，字符串不带结尾 NUL。新增 tag 不影响本解析器。
 *
 * 请求的 recipient 是 device，按 WebUSB 规范不需要 claimInterface ——
 * 这是本页面能与 pyOCD/OpenOCD 同时访问同一台设备的前提。
 *
 * 契约的唯一权威在固件侧 main/config_channel.cc 顶部注释。
 */

export const VID = 0x303a;
export const PID = 0x4020;

export const VENDOR_CODE = 0x4d;
export const CMD_GET_STATS = 0x01;
export const CMD_GET_CONFIG = 0x02;
export const CMD_GET_USB_CONFIG = 0x03;
export const CMD_SET_MODE = 0x10;
export const CMD_ENTER_PAIRING = 0x11;
export const CMD_CLEAR_PAIR = 0x12;
export const CMD_RESET = 0x13;
export const CMD_REBOOT = 0x14;
export const CMD_EXIT_PAIRING = 0x15;
export const CMD_SET_USB_CONFIG = 0x16;
export const CMD_APPLY_USB_CONFIG = 0x17;
export const CMD_PING = 0x18;
export const CMD_SET_WIFI_RATE = 0x19;
export const CMD_SET_LOSS = 0x1a;
export const CMD_SET_LINK_CONFIG = 0x1b;
/**
 * 软复位进更新模式：设备重启后 USB 只暴露一个 CDC 串口（产品名 MeguminLink Update），
 * 升级协议跑在那个口上（见 update.ts）。硬复位即退回正常模式。
 */
export const CMD_ENTER_UPDATE = 0x1c;
export const CMD_SET_UART_RELIABLE = 0x1d;
/** 固件的静态响应缓冲是 256B，实测整帧 77B；这里给足余量。 */
export const MAX_RESPONSE = 512;

const FRAME_MAGIC = 0x4d4c;
const FRAME_HEADER_SIZE = 8;

export const STATUS_NAMES: Readonly<Record<number, string>> = {
	0x00: 'ok',
	0x01: 'unknown-command',
	0x02: 'malformed',
	0x03: 'internal',
	0x04: 'unsupported',
	0x05: 'busy',
};

export const MODE_NAMES: Readonly<Record<number, string>> = { 0: 'local', 1: 'wireless' };
export const PAIR_NAMES: Readonly<Record<number, string>> = { 0: 'idle', 1: 'paired', 2: 'pairing' };
/** SetMode 的参数取值，与固件 Mode 枚举一致。 */
export const MODE_LOCAL = 0;
export const MODE_WIRELESS = 1;
/** Stats.pair 的取值：仅已配对时才有可探测的对端。 */
export const PAIR_PAIRED = 1;

/** GetConfig 响应的 tag：0x41-0x43 设备身份，0x50-0x53 配置项。 */
const TAG_FW_VERSION = 0x41;
const TAG_CHIP_TARGET = 0x42;
const TAG_IDF_VERSION = 0x43;
const TAG_CONFIG_MODE = 0x50;
const TAG_CONFIG_PEER_MAC = 0x51;
const TAG_CONFIG_WIFI_RATE = 0x52;
const TAG_CONFIG_LOSS_PERMILLE = 0x53;

/** CDC ACM 串口状态的 tag（0x30-0x33 是 UART 字节计数器，这里从 0x34 续编）。 */
const TAG_UART_HOST_OPEN = 0x34;
const TAG_UART_BAUD = 0x35;
const TAG_UART_DATA_BITS = 0x36;
const TAG_UART_STOP_BITS = 0x37;
const TAG_UART_PARITY = 0x38;
const TAG_UART_RELIABLE = 0x39;

/** 对端存活探测的 tag（0x20-0x2F 无线段，紧跟空口字节计数器续编）。 */
const TAG_PEER_ONLINE = 0x22;
const TAG_PING_RTT = 0x23;
/** 丢包注入的计数器（同在 0x20-0x2F 无线段；固件未编译该功能时不下发）。 */
const TAG_LOSS_FRAMES = 0x26;
const TAG_LOSS_DROPPED = 0x27;

/**
 * 对端存活状态（Stats.peerOnline）。
 * UNKNOWN 是「从未探测」——设备刚上电或刚配对时如此，界面显示「—」而不是
 * 把未探测当成离线。
 */
export const PEER_UNKNOWN = 0;
export const PEER_ONLINE = 1;
export const PEER_OFFLINE = 2;

/**
 * 可靠传输的 tag。参数在 GetConfig 的 0x54-0x5D（持久化配置项段），运行时状态与
 * 计数在 GetStats 的 0x70-0x7C（无线段 0x20-0x2F 已排满，单开一段）。
 * 固件未编译该功能时整组不下发，界面显示「—」。
 */
const TAG_MEAPROTO_RTO_MIN = 0x59;
const TAG_MEAPROTO_RTO_INIT = 0x5a;
const TAG_MEAPROTO_RTO_MAX = 0x5b;
const TAG_MEAPROTO_DELAYED_ACK = 0x5c;
const TAG_MEAPROTO_PROBE = 0x5d;

const TAG_MEAPROTO_STATE = 0x70;
const TAG_MEAPROTO_TX = 0x71;
const TAG_MEAPROTO_RETX = 0x72;
const TAG_MEAPROTO_ACKED = 0x73;
const TAG_MEAPROTO_FAILED = 0x74;
const TAG_MEAPROTO_DUP = 0x75;
const TAG_MEAPROTO_OOO = 0x76;
const TAG_MEAPROTO_SKIPPED = 0x77;
const TAG_MEAPROTO_REPLAYED = 0x78;
const TAG_MEAPROTO_REPLAY_MISS = 0x79;
const TAG_MEAPROTO_SRTT_MS = 0x7a;
const TAG_MEAPROTO_RTO_MS = 0x7b;

/** SetLinkConfig 的 OUT 载荷自成一套 TLV 空间，只接受运行期可改的时序类字段。 */
// 0x01-0x05 未分配
const LINK_TAG_RTO_INIT = 0x06;
const LINK_TAG_RTO_MAX = 0x07;
const LINK_TAG_DELAYED_ACK = 0x08;
const LINK_TAG_PROBE = 0x09;

/**
 * 状态字节（0x70）的位域：bit0-3 生效窗口、bit4-6 在途消息数。
 * 在途数与窗口打包成一个字节，是为了省一个 TLV；窗口上限 8、在途上限 7 都装得下。
 */
export const MEAPROTO_STATE_WINDOW_MASK = 0x0f;
export const MEAPROTO_STATE_IN_FLIGHT_SHIFT = 4;
export const MEAPROTO_STATE_IN_FLIGHT_MASK = 0x07;

/** GetUsbConfig / SetUsbConfig 的 tag（0x60-0x6F USB 能力）。 */
const TAG_USB_SUPPORTED = 0x60;
const TAG_USB_WEBUSB = 0x61;
const TAG_USB_WINUSB = 0x62;
const TAG_USB_LANDING_PAGE = 0x63;
const TAG_USB_DRAFT = 0x64;
const TAG_USB_DRAFT_REASON = 0x65;
const TAG_USB_HID_COMPAT = 0x66;
const TAG_USB_DEVICE_NAME = 0x67;

/** 编译期支持的能力位（UsbConfig.supported）。 */
export const USB_CAPABILITY_WEBUSB = 0x01;
export const USB_CAPABILITY_WINUSB = 0x02;
export const USB_CAPABILITY_HID_COMPAT = 0x04;
/** landing page 字符上限，与固件 usb_capability.hpp 一致。 */
export const USB_LANDING_PAGE_MAX_LEN = 126;
/** 设备名字符上限，与固件 usb_capability.hpp 一致。 */
export const USB_DEVICE_NAME_MAX_LEN = 20;
/** 草稿状态（UsbConfig.draft）：0 无草稿、1 合法待应用、2 被拒。 */
export const USB_DRAFT_PENDING = 1;
export const USB_DRAFT_REJECTED = 2;
/** 被拒原因（UsbConfig.draftReason）的中文说明：原因码由 URL 与设备名共用。 */
export const USB_REJECT_TEXT: Readonly<Record<number, string>> = {
	1: `超过长度上限（URL ${USB_LANDING_PAGE_MAX_LEN} 字符 / 设备名称 ${USB_DEVICE_NAME_MAX_LEN} 字符）`,
	2: '只能包含可打印 ASCII 字符',
	3: '该能力在当前固件里未编译进来',
};

/**
 * 统计快照。计数器一律 u32 累计值，回绕由上层按 (cur - prev) >>> 0 处理。
 *
 * uart* 五项是 CDC ACM 串口状态（不在 STAT_FIELDS 里，因而也不参与速率差分）：
 * 老固件不上报这些 tag，此时为 null，界面显示「—」而不是把它们当成 0。
 *
 * peerOnline / pingRttUs 是对端存活探测的结果：online 由协议连接状态给出
 * （老固件为 null 或 PEER_UNKNOWN），rttUs 是协议层平滑估计的往返（微秒）。
 * 同样不进速率差分：它们描述链路状态，不是链路吞吐。
 */
export type Stats = {
	mode: number;
	pair: number;
	rssi: number;
	dapCmds: number;
	dapReqBytes: number;
	dapRespBytes: number;
	wlTxBytes: number;
	wlRxBytes: number;
	uartRxBytes: number;
	uartTxBytes: number;
	uartWlRxBytes: number;
	uartWlTxBytes: number;
	uptimeMs: number;
	uartHostOpen: number | null;
	uartBaud: number | null;
	uartDataBits: number | null;
	uartStopBits: number | null;
	uartParity: number | null;
	/** 串口出向是否走可靠传输：1 = 可靠，0 = 尽力而为；老固件不上报时为 null */
	uartReliable: number | null;
	peerOnline: number | null;
	pingRttUs: number | null;
	/**
	 * 丢包注入的读数：lossFrames 是过白名单、参与判定的出向帧数，lossDropped
	 * 是其中被判丢的帧数，两者之比即实际注入比例。固件未编译该功能时为 null。
	 */
	lossFrames: number | null;
	lossDropped: number | null;
	/**
	 * 可靠传输的运行状态与计数（同一次快照）。meaprotoState 是打包字节，用上面那组位掩码
	 * 拆；meaprotoRetx / meaprotoTx 之比即重传率，是"链路现在有多差"最直观的读数。
	 * 固件未编译该功能时为 null。
	 */
	meaprotoState: number | null;
	meaprotoTx: number | null;
	meaprotoRetx: number | null;
	meaprotoAcked: number | null;
	meaprotoFailed: number | null;
	meaprotoDup: number | null;
	meaprotoOoo: number | null;
	meaprotoSkipped: number | null;
	meaprotoReplayed: number | null;
	meaprotoReplayMiss: number | null;
	meaprotoSrttMs: number | null;
	meaprotoRtoMs: number | null;
};

/** 可靠传输那组可空字段的键（不参与速率差分，见 rate.ts 的 CounterKey）。 */
export type MeaprotoStateKey =
	| 'meaprotoState'
	| 'meaprotoTx'
	| 'meaprotoRetx'
	| 'meaprotoAcked'
	| 'meaprotoFailed'
	| 'meaprotoDup'
	| 'meaprotoOoo'
	| 'meaprotoSkipped'
	| 'meaprotoReplayed'
	| 'meaprotoReplayMiss'
	| 'meaprotoSrttMs'
	| 'meaprotoRtoMs';

/** SetLinkConfig 的载荷：只带要改的字段，undefined = 保持设备当前值。 */
export type LinkConfigPatch = {
	rtoInitMs?: number;
	rtoMaxMs?: number;
	delayedAckMs?: number;
	probeMs?: number;
};

/** 重传率（百分比）；一次都没发出去过时返回 null（显示「—」而不是 0%）。 */
export function meaprotoRetxRate(stats: Stats): number | null {
	if (stats.meaprotoTx === null || stats.meaprotoTx === 0) return null;
	return ((stats.meaprotoRetx ?? 0) / stats.meaprotoTx) * 100;
}

export type StatField = {
	tag: number;
	key: keyof Stats;
	/** 与 mlinkctl.py 的输出字段名一致，便于两边对照。 */
	name: string;
	label: string;
	signed?: boolean;
};

/**
 * ESP-NOW 发送速率档位的取值域（wifi_phy_rate_t 原值）与显示名。
 *
 * 档位越高越依赖信噪比，长帧的误帧率随之上升。本链路的实测吞吐与档位无关——
 * 瓶颈是每条 DAP 命令的往返延迟（~15ms/条）而不是空口时间——所以高档位只
 * 换来丢帧风险，没有换来速度。分组按调制族划分，组内按标称速率升序。
 */
export const WIFI_RATE_GROUPS: { group: string; rates: { value: number; label: string }[] }[] = [
	{
		group: '11b（CCK）',
		rates: [
			{ value: 0x00, label: '1 Mbps · 长前导' },
			{ value: 0x01, label: '2 Mbps · 长前导' },
			{ value: 0x02, label: '5.5 Mbps · 长前导' },
			{ value: 0x03, label: '11 Mbps · 长前导' },
			{ value: 0x05, label: '2 Mbps · 短前导' },
			{ value: 0x06, label: '5.5 Mbps · 短前导' },
			{ value: 0x07, label: '11 Mbps · 短前导' },
		],
	},
	{
		group: '11g（OFDM）',
		rates: [
			{ value: 0x0b, label: '6 Mbps' },
			{ value: 0x0f, label: '9 Mbps' },
			{ value: 0x0a, label: '12 Mbps' },
			{ value: 0x0e, label: '18 Mbps' },
			{ value: 0x09, label: '24 Mbps（固件默认）' },
			{ value: 0x0d, label: '36 Mbps' },
			{ value: 0x08, label: '48 Mbps' },
			{ value: 0x0c, label: '54 Mbps' },
		],
	},
	{
		group: 'HT20 MCS（长保护间隔）',
		rates: [
			{ value: 0x10, label: 'MCS0 · 6.5 Mbps' },
			{ value: 0x11, label: 'MCS1 · 13 Mbps' },
			{ value: 0x12, label: 'MCS2 · 19.5 Mbps' },
			{ value: 0x13, label: 'MCS3 · 26 Mbps' },
			{ value: 0x14, label: 'MCS4 · 39 Mbps' },
			{ value: 0x15, label: 'MCS5 · 52 Mbps' },
			{ value: 0x16, label: 'MCS6 · 58.5 Mbps' },
			{ value: 0x17, label: 'MCS7 · 65 Mbps' },
		],
	},
	{
		group: 'HT20 MCS（短保护间隔）',
		rates: [
			{ value: 0x18, label: 'MCS0 · 7.2 Mbps' },
			{ value: 0x19, label: 'MCS1 · 14.4 Mbps' },
			{ value: 0x1a, label: 'MCS2 · 21.7 Mbps' },
			{ value: 0x1b, label: 'MCS3 · 28.9 Mbps' },
			{ value: 0x1c, label: 'MCS4 · 43.3 Mbps' },
			{ value: 0x1d, label: 'MCS5 · 57.8 Mbps' },
			{ value: 0x1e, label: 'MCS6 · 65 Mbps' },
			{ value: 0x1f, label: 'MCS7 · 72.2 Mbps' },
		],
	},
];

/** 档位原值 → 显示名；不在取值域内（旧固件或无值）时返回 null。 */
export const WIFI_RATE_NAMES: ReadonlyMap<number, string> = new Map(
	WIFI_RATE_GROUPS.flatMap(({ rates }) => rates.map(({ value, label }) => [value, label] as const))
);

/**
 * 注入丢包率的界面单位（百分比）与换算。
 *
 * 设备端按千分比存放（配置通道给命令参数只留了 8 位，上限 255‰），界面与
 * CLI 统一按百分比呈现：0-25.5%，步进 0.1%（1‰）。
 */
export const LOSS_PERCENT_MAX = 25.5;
export const LOSS_PERMILLE_PER_PERCENT = 10;

/** 千分比 → 显示文本（0.1% 步进，固定一位小数）。 */
export function lossPercentText(permille: number): string {
	return `${(permille / LOSS_PERMILLE_PER_PERCENT).toFixed(1)}%`;
}

/**
 * 设备身份与配置项快照（GetConfig）。
 *
 * mode 与 Stats.mode 同值域，peerMac 为小写冒号分隔的 6 字节 MAC，
 * 未配对时为 null。wifiRate 是 ESP-NOW 发送速率档位（wifi_phy_rate_t 原值），
 * 不支持该配置的旧固件为 null。
 *
 * lossPermille 是注入丢包率，设备端按千分比存放（0-255，0.1% 步进），界面
 * 上按百分比呈现；固件未编译该功能时既不下发该配置也不认识 0x1A 命令，
 * 此时为 null。
 */
export type DeviceConfig = {
	fwVersion: string;
	chipTarget: string;
	idfVersion: string;
	mode: number;
	peerMac: string | null;
	wifiRate: number | null;
	lossPermille: number | null;
	/** 链路时序参数（0x59-0x5D）；固件未编译该功能时为 null。 */
	meaprotoRtoMinMs: number | null;
	meaprotoRtoInitMs: number | null;
	meaprotoRtoMaxMs: number | null;
	meaprotoDelayedAckMs: number | null;
};

/**
 * USB 能力快照（GetUsbConfig）。
 *
 * supported 是编译期支持位（bit0 WebUSB / bit1 WinUSB / bit2 HID 兼容模式）：
 * 未编译进来的能力在界面上置灰。landingPage 为生效值（设备端已把空串解析成默认
 * URL），deviceName 是生效的设备名（空串表示不用自定义名），draft 为草稿状态，
 * draftReason 只在被拒时有值。
 */
export type UsbConfig = {
	supported: number;
	webusb: number;
	winusb: number;
	hidCompat: number;
	landingPage: string;
	deviceName: string;
	draft: number;
	draftReason: number | null;
};

/** SetUsbConfig（0x16）提交的完整配置；landingPage 传空串表示回退默认值，deviceName 传空串表示不用自定义名。 */
export type UsbCapability = {
	webusb: number;
	winusb: number;
	hidCompat: number;
	landingPage: string;
	deviceName: string;
};

/** 字段顺序即 mlinkctl 的打印顺序。 */
export const STAT_FIELDS: readonly StatField[] = [
	{ tag: 0x01, key: 'mode', name: 'mode', label: '链路模式' },
	{ tag: 0x02, key: 'pair', name: 'pair', label: '配对状态' },
	{ tag: 0x03, key: 'rssi', name: 'rssi', label: 'RSSI', signed: true },
	{ tag: 0x10, key: 'dapCmds', name: 'dap_cmds', label: 'DAP 命令数' },
	{ tag: 0x11, key: 'dapReqBytes', name: 'dap_req_bytes', label: 'DAP 请求字节' },
	{ tag: 0x12, key: 'dapRespBytes', name: 'dap_resp_bytes', label: 'DAP 响应字节' },
	{ tag: 0x20, key: 'wlTxBytes', name: 'wl_tx_bytes', label: '无线发送字节' },
	{ tag: 0x21, key: 'wlRxBytes', name: 'wl_rx_bytes', label: '无线接收字节' },
	{ tag: 0x30, key: 'uartRxBytes', name: 'uart_rx_bytes', label: 'UART 接收字节（Local）' },
	{ tag: 0x31, key: 'uartTxBytes', name: 'uart_tx_bytes', label: 'UART 发送字节（Local）' },
	{ tag: 0x32, key: 'uartWlRxBytes', name: 'uart_wl_rx_bytes', label: 'UART 接收字节（Wireless）' },
	{ tag: 0x33, key: 'uartWlTxBytes', name: 'uart_wl_tx_bytes', label: 'UART 发送字节（Wireless）' },
	{ tag: 0x40, key: 'uptimeMs', name: 'uptime_ms', label: '开机时长' },
];

export class ProtocolError extends Error {
	/** 设备返回的状态码；传输层失败时为 null。 */
	readonly status: number | null;

	constructor(message: string, status: number | null = null) {
		super(message);
		this.name = 'ProtocolError';
		this.status = status;
	}
}

function parseTlv(frame: DataView, start: number, end: number): Map<number, DataView> {
	const tags = new Map<number, DataView>();
	let pos = start;
	while (pos + 2 <= end) {
		const tag = frame.getUint8(pos);
		let length = frame.getUint8(pos + 1);
		pos += 2;
		if (length === 0xff) {
			if (pos + 2 > end) break;
			length = frame.getUint16(pos, false);
			pos += 2;
		}
		if (pos + length > end) break;
		tags.set(tag, new DataView(frame.buffer, frame.byteOffset + pos, length));
		pos += length;
	}
	return tags;
}

/**
 * CDC ACM 串口状态字段：它们是状态而不是计数器，既不进 STAT_FIELDS，
 * 也不参与 rate.ts 的速率差分（CounterKey 显式把它们排除在外）。
 */
export type UartStateKey =
	| 'uartHostOpen'
	| 'uartBaud'
	| 'uartDataBits'
	| 'uartStopBits'
	| 'uartParity'
	| 'uartReliable';

/** 对端存活探测字段：与串口状态同待遇，既不进 STAT_FIELDS 也不参与速率差分。 */
export type PeerStateKey = 'peerOnline' | 'pingRttUs';

/**
 * 丢包注入的读数字段：同样是实验读数而不是链路吞吐，既不进 STAT_FIELDS
 * （不混进主统计表）也不参与速率差分。
 */
export type LossStateKey = 'lossFrames' | 'lossDropped';

/** 缺失的 tag 一律为 null：区分"设备上报 0"与"设备根本没上报这个字段"。 */
function readOptional(view: DataView | undefined): number | null {
	if (view === undefined || view.byteLength === 0) return null;
	return readValue(view, false);
}

function readValue(view: DataView | undefined, signed: boolean): number {
	if (view === undefined || view.byteLength === 0) return 0;
	switch (view.byteLength) {
		case 1:
			return signed ? view.getInt8(0) : view.getUint8(0);
		case 2:
			return signed ? view.getInt16(0, false) : view.getUint16(0, false);
		case 4:
			return signed ? view.getInt32(0, false) : view.getUint32(0, false);
		default:
			throw new ProtocolError(`unexpected TLV length ${view.byteLength}`);
	}
}

/** 校验帧头与 status，返回 TLV 区（键为 tag）。 */
function parseFrame(frame: DataView): Map<number, DataView> {
	if (frame.byteLength < FRAME_HEADER_SIZE) {
		throw new ProtocolError(`short response (${frame.byteLength} bytes)`);
	}
	if (frame.getUint16(0, false) !== FRAME_MAGIC) {
		throw new ProtocolError('bad frame magic');
	}
	const status = frame.getUint8(6);
	if (status !== 0x00) {
		const name = STATUS_NAMES[status] ?? `0x${status.toString(16)}`;
		throw new ProtocolError(`device returned status=${name}`, status);
	}
	const declared = frame.getUint16(4, false);
	const end = Math.min(FRAME_HEADER_SIZE + declared, frame.byteLength);
	return parseTlv(frame, FRAME_HEADER_SIZE, end);
}

function readText(view: DataView | undefined): string {
	if (view === undefined || view.byteLength === 0) return '';
	return new TextDecoder().decode(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
}

function readMac(view: DataView | undefined): string | null {
	if (view === undefined || view.byteLength !== 6) return null;
	const octets: string[] = [];
	for (let i = 0; i < 6; i += 1) {
		octets.push(view.getUint8(i).toString(16).padStart(2, '0'));
	}
	return octets.join(':');
}

export function decodeStats(frame: DataView): Stats {
	const tags = parseFrame(frame);
	const stats = {} as Stats;
	for (const field of STAT_FIELDS) {
		stats[field.key] = readValue(tags.get(field.tag), field.signed === true);
	}
	stats.uartHostOpen = readOptional(tags.get(TAG_UART_HOST_OPEN));
	stats.uartBaud = readOptional(tags.get(TAG_UART_BAUD));
	stats.uartDataBits = readOptional(tags.get(TAG_UART_DATA_BITS));
	stats.uartStopBits = readOptional(tags.get(TAG_UART_STOP_BITS));
	stats.uartParity = readOptional(tags.get(TAG_UART_PARITY));
	stats.uartReliable = readOptional(tags.get(TAG_UART_RELIABLE));
	stats.peerOnline = readOptional(tags.get(TAG_PEER_ONLINE));
	stats.pingRttUs = readOptional(tags.get(TAG_PING_RTT));
	stats.lossFrames = readOptional(tags.get(TAG_LOSS_FRAMES));
	stats.lossDropped = readOptional(tags.get(TAG_LOSS_DROPPED));
	stats.meaprotoState = readOptional(tags.get(TAG_MEAPROTO_STATE));
	stats.meaprotoTx = readOptional(tags.get(TAG_MEAPROTO_TX));
	stats.meaprotoRetx = readOptional(tags.get(TAG_MEAPROTO_RETX));
	stats.meaprotoAcked = readOptional(tags.get(TAG_MEAPROTO_ACKED));
	stats.meaprotoFailed = readOptional(tags.get(TAG_MEAPROTO_FAILED));
	stats.meaprotoDup = readOptional(tags.get(TAG_MEAPROTO_DUP));
	stats.meaprotoOoo = readOptional(tags.get(TAG_MEAPROTO_OOO));
	stats.meaprotoSkipped = readOptional(tags.get(TAG_MEAPROTO_SKIPPED));
	stats.meaprotoReplayed = readOptional(tags.get(TAG_MEAPROTO_REPLAYED));
	stats.meaprotoReplayMiss = readOptional(tags.get(TAG_MEAPROTO_REPLAY_MISS));
	stats.meaprotoSrttMs = readOptional(tags.get(TAG_MEAPROTO_SRTT_MS));
	stats.meaprotoRtoMs = readOptional(tags.get(TAG_MEAPROTO_RTO_MS));
	return stats;
}

export function decodeConfig(frame: DataView): DeviceConfig {
	const tags = parseFrame(frame);
	return {
		fwVersion: readText(tags.get(TAG_FW_VERSION)),
		chipTarget: readText(tags.get(TAG_CHIP_TARGET)),
		idfVersion: readText(tags.get(TAG_IDF_VERSION)),
		mode: readValue(tags.get(TAG_CONFIG_MODE), false),
		peerMac: readMac(tags.get(TAG_CONFIG_PEER_MAC)),
		wifiRate: readOptional(tags.get(TAG_CONFIG_WIFI_RATE)),
		lossPermille: readOptional(tags.get(TAG_CONFIG_LOSS_PERMILLE)),
		meaprotoRtoMinMs: readOptional(tags.get(TAG_MEAPROTO_RTO_MIN)),
		meaprotoRtoInitMs: readOptional(tags.get(TAG_MEAPROTO_RTO_INIT)),
		meaprotoRtoMaxMs: readOptional(tags.get(TAG_MEAPROTO_RTO_MAX)),
		meaprotoDelayedAckMs: readOptional(tags.get(TAG_MEAPROTO_DELAYED_ACK)),
	};
}

export function decodeUsbConfig(frame: DataView): UsbConfig {
	const tags = parseFrame(frame);
	const reason = tags.get(TAG_USB_DRAFT_REASON);
	return {
		supported: readValue(tags.get(TAG_USB_SUPPORTED), false),
		webusb: readValue(tags.get(TAG_USB_WEBUSB), false),
		winusb: readValue(tags.get(TAG_USB_WINUSB), false),
		hidCompat: readValue(tags.get(TAG_USB_HID_COMPAT), false),
		landingPage: readText(tags.get(TAG_USB_LANDING_PAGE)),
		deviceName: readText(tags.get(TAG_USB_DEVICE_NAME)),
		draft: readValue(tags.get(TAG_USB_DRAFT), false),
		draftReason: reason === undefined ? null : readValue(reason, false),
	};
}

/**
 * 发一次厂商 IN 请求并取回响应帧。
 *
 * wValue 低字节是命令码、高字节是命令参数（与固件同一约定）；seq 仅用于
 * 诊断请求/响应错位，设备原样回显。
 */
async function transferIn(device: USBDevice, wValue: number, seq: number): Promise<DataView> {
	const result = await device.controlTransferIn(
		{
			requestType: 'vendor',
			recipient: 'device',
			request: VENDOR_CODE,
			value: wValue,
			index: seq & 0xff,
		},
		MAX_RESPONSE
	);
	if (result.status !== 'ok') {
		throw new ProtocolError(`transfer ${result.status}`);
	}
	if (!result.data) {
		throw new ProtocolError('empty response');
	}
	return result.data;
}

export async function readStats(device: USBDevice, seq: number): Promise<Stats> {
	return decodeStats(await transferIn(device, CMD_GET_STATS, seq));
}

export async function readConfig(device: USBDevice, seq: number): Promise<DeviceConfig> {
	return decodeConfig(await transferIn(device, CMD_GET_CONFIG, seq));
}

/**
 * 下发一条控制命令。
 *
 * 动作命令的响应帧 TLV 区为空：status == ok 只表示设备已接受投递，动作由
 * 设备主循环执行（结果看后续轮询的模式/配对状态）；busy 表示设备端还有
 * 未取走的动作。status != ok 与传输失败都由 parseFrame 抛 ProtocolError。
 */
export async function runCommand(device: USBDevice, cmd: number, seq: number, arg = 0): Promise<void> {
	parseFrame(await transferIn(device, cmd | (arg << 8), seq));
}

export async function readUsbConfig(device: USBDevice, seq: number): Promise<UsbConfig> {
	return decodeUsbConfig(await transferIn(device, CMD_GET_USB_CONFIG, seq));
}

/**
 * 触发一次对端存活探测（cmd 0x18）。
 *
 * 与其它动作命令同语义：只表示设备已接受投递，探测由协议层发出 Ping 并在
 * 收到 Pong 后刷新平滑往返；结果不在本次响应里返回，随后的 readStats 读
 * peerOnline 与 pingRttUs 即可。
 */
export async function pingPeer(device: USBDevice, seq: number): Promise<void> {
	parseFrame(await transferIn(device, CMD_PING, seq));
}

function encodeTlv(tag: number, value: Uint8Array): Uint8Array<ArrayBuffer> {
	const header =
		value.byteLength >= 0xff
			? new Uint8Array([tag, 0xff, value.byteLength >> 8, value.byteLength & 0xff])
			: new Uint8Array([tag, value.byteLength]);
	const out = new Uint8Array(header.byteLength + value.byteLength);
	out.set(header, 0);
	out.set(value, header.byteLength);
	return out;
}

function concatBytes(parts: Uint8Array[]): Uint8Array<ArrayBuffer> {
	const total = parts.reduce((sum, part) => sum + part.byteLength, 0);
	const out = new Uint8Array(total);
	let offset = 0;
	for (const part of parts) {
		out.set(part, offset);
		offset += part.byteLength;
	}
	return out;
}

/**
 * landing page 的本地预校验：与固件同规则（长度 + 可打印 ASCII）。
 * 返回 null 表示合法；空串合法（设备端会解析成编译期默认值）。
 */
export function landingPageError(url: string): string | null {
	const bytes = new TextEncoder().encode(url);
	if (bytes.byteLength > USB_LANDING_PAGE_MAX_LEN) return `URL 超过 ${USB_LANDING_PAGE_MAX_LEN} 字符`;
	for (const byte of bytes) {
		if (byte < 0x20 || byte > 0x7e) return 'URL 只能包含可打印 ASCII 字符';
	}
	return null;
}

/**
 * 设备名的本地预校验：与固件同规则（长度 + 可打印 ASCII）。
 * 返回 null 表示合法；空串合法（表示不用自定义名）。
 */
export function deviceNameError(name: string): string | null {
	const bytes = new TextEncoder().encode(name);
	if (bytes.byteLength > USB_DEVICE_NAME_MAX_LEN) return `设备名称超过 ${USB_DEVICE_NAME_MAX_LEN} 字符`;
	for (const byte of bytes) {
		if (byte < 0x20 || byte > 0x7e) return '设备名称只能包含可打印 ASCII 字符';
	}
	return null;
}

/**
 * 提交 USB 能力草稿（cmd 0x16，OUT 方向）。
 *
 * 载荷是全量三项的 TLV（WebUSB / WinUSB / landing page），设备端只校验并缓存，
 * 不落盘不生效；结果用随后的 readUsbConfig 读草稿状态确认。
 */
/**
 * 下发可靠传输参数（SetLinkConfig，OUT 方向）。
 *
 * 只把 patch 里给出的字段编进载荷——设备端按"只覆盖出现的字段"处理，没给的保持它
 * 当前值；参数代数由设备自己 +1。应用在设备主循环里做，因此本函数返回只代表载荷
 * 已被接受，生效值与新一代代数要随后读一次 GetConfig。
 */
export async function writeLinkConfig(
	device: USBDevice,
	seq: number,
	patch: LinkConfigPatch
): Promise<void> {
	const u16 = (tag: number, value: number): Uint8Array<ArrayBuffer> =>
		encodeTlv(tag, new Uint8Array([(value >> 8) & 0xff, value & 0xff]));

	const parts: Uint8Array[] = [];
	if (patch.rtoInitMs !== undefined) parts.push(u16(LINK_TAG_RTO_INIT, patch.rtoInitMs));
	if (patch.rtoMaxMs !== undefined) parts.push(u16(LINK_TAG_RTO_MAX, patch.rtoMaxMs));
	if (patch.delayedAckMs !== undefined) parts.push(u16(LINK_TAG_DELAYED_ACK, patch.delayedAckMs));
	if (patch.probeMs !== undefined) parts.push(u16(LINK_TAG_PROBE, patch.probeMs));

	if (parts.length === 0) {
		throw new ProtocolError('empty link config patch');
	}

	const result = await device.controlTransferOut(
		{
			requestType: 'vendor',
			recipient: 'device',
			request: VENDOR_CODE,
			value: CMD_SET_LINK_CONFIG,
			index: seq & 0xff,
		},
		concatBytes(parts)
	);
	if (result.status !== 'ok') {
		throw new ProtocolError(`transfer ${result.status}`);
	}
}

export async function writeUsbConfig(device: USBDevice, seq: number, config: UsbCapability): Promise<void> {
	const encoder = new TextEncoder();
	const payload = concatBytes([
		encodeTlv(TAG_USB_WEBUSB, new Uint8Array([config.webusb & 1])),
		encodeTlv(TAG_USB_WINUSB, new Uint8Array([config.winusb & 1])),
		encodeTlv(TAG_USB_HID_COMPAT, new Uint8Array([config.hidCompat & 1])),
		encodeTlv(TAG_USB_LANDING_PAGE, encoder.encode(config.landingPage)),
		encodeTlv(TAG_USB_DEVICE_NAME, encoder.encode(config.deviceName)),
	]);

	const result = await device.controlTransferOut(
		{
			requestType: 'vendor',
			recipient: 'device',
			request: VENDOR_CODE,
			value: CMD_SET_USB_CONFIG,
			index: seq & 0xff,
		},
		payload
	);
	if (result.status !== 'ok') {
		throw new ProtocolError(`transfer ${result.status}`);
	}
}

/** 应用草稿（cmd 0x17）：设备落盘、重建描述符并重新枚举 USB。 */
export async function applyUsbConfig(device: USBDevice, seq: number): Promise<void> {
	parseFrame(await transferIn(device, CMD_APPLY_USB_CONFIG, seq));
}
