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
export const CMD_SET_MODE = 0x10;
export const CMD_ENTER_PAIRING = 0x11;
export const CMD_CLEAR_PAIR = 0x12;
export const CMD_RESET = 0x13;
export const CMD_REBOOT = 0x14;
export const CMD_EXIT_PAIRING = 0x15;
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

/** GetConfig 响应的 tag：0x41-0x43 设备身份，0x50/0x51 配置项。 */
const TAG_FW_VERSION = 0x41;
const TAG_CHIP_TARGET = 0x42;
const TAG_IDF_VERSION = 0x43;
const TAG_CONFIG_MODE = 0x50;
const TAG_CONFIG_PEER_MAC = 0x51;

/** 统计快照。计数器一律 u32 累计值，回绕由上层按 (cur - prev) >>> 0 处理。 */
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
};

export type StatField = {
	tag: number;
	key: keyof Stats;
	/** 与 mlinkctl.py 的输出字段名一致，便于两边对照。 */
	name: string;
	label: string;
	signed?: boolean;
};

/**
 * 设备身份与配置项快照（GetConfig）。
 *
 * mode 与 Stats.mode 同值域，peerMac 为小写冒号分隔的 6 字节 MAC，
 * 未配对时为 null。
 */
export type DeviceConfig = {
	fwVersion: string;
	chipTarget: string;
	idfVersion: string;
	mode: number;
	peerMac: string | null;
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
