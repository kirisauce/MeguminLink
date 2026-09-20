/**
 * 速率差分与显示格式化。
 *
 * 速率的分母取设备自己的 uptime_ms 差分，而不是主机墙钟：主机侧定时器会被
 * 调度抖动与后台节流影响，用设备时钟才能得到真实的平均速率。
 * 计数器是 u32 累计值，差分按 (cur - prev) >>> 0 处理回绕；
 * Δt 为 0 或超过 MAX_GAP_MS 时（设备重启、长时间挂起）本次速率判为无效。
 */

import {
	STAT_FIELDS,
	type ArqStateKey,
	type LossStateKey,
	type PeerStateKey,
	type Stats,
	type UartStateKey
} from './protocol';

/** 一次采样跨越这么久就认为数据不再连续（设备重启或页面长时间挂起）。 */
export const MAX_GAP_MS = 2000;

/**
 * 参与速率计算的字段：STAT_FIELDS 里除状态位与 uptime 之外的全部计数器。
 * 串口状态（UartStateKey）、对端探测结果（PeerStateKey）、丢包注入读数
 * （LossStateKey）与可靠传输状态（ArqStateKey）不在 STAT_FIELDS 里，这里再从
 * 类型上排除一次。
 */
export type CounterKey = Exclude<
	keyof Stats,
	'mode' | 'pair' | 'rssi' | 'uptimeMs' | UartStateKey | PeerStateKey | LossStateKey | ArqStateKey
>;

export const COUNTER_KEYS = STAT_FIELDS.filter(
	(field) => field.key !== 'mode' && field.key !== 'pair' && field.key !== 'rssi' && field.key !== 'uptimeMs'
).map((field) => field.key as CounterKey);

/** 各计数器的每秒增量。 */
export type Rates = Record<CounterKey, number>;

export function computeRates(prev: Stats, cur: Stats): Rates | null {
	const dtMs = (cur.uptimeMs - prev.uptimeMs) >>> 0;
	if (dtMs === 0 || dtMs > MAX_GAP_MS) return null;
	const seconds = dtMs / 1000;
	const rates = {} as Rates;
	for (const key of COUNTER_KEYS) {
		rates[key] = ((cur[key] - prev[key]) >>> 0) / seconds;
	}
	return rates;
}

const UNITS = ['B', 'KiB', 'MiB', 'GiB'];

/** 1024 进制、三位有效数字，用于累计字节。 */
export function formatBytes(value: number): string {
	if (!Number.isFinite(value)) return '—';
	let scaled = value;
	let unit = 0;
	while (scaled >= 1024 && unit < UNITS.length - 1) {
		scaled /= 1024;
		unit += 1;
	}
	return `${formatScaled(scaled)} ${UNITS[unit]}`;
}

/** 与 formatBytes 同刻度，后缀为 /s。 */
export function formatRate(value: number): string {
	if (!Number.isFinite(value)) return '—';
	let scaled = value;
	let unit = 0;
	while (scaled >= 1024 && unit < UNITS.length - 1) {
		scaled /= 1024;
		unit += 1;
	}
	return `${formatScaled(scaled)} ${UNITS[unit]}/s`;
}

/** 轴刻度用：1024 进制缩写，不带单位，避免刻度文字过长。 */
export function formatCompact(value: number): string {
	if (!Number.isFinite(value)) return '';
	let scaled = Math.abs(value);
	let unit = 0;
	while (scaled >= 1024 && unit < UNITS.length - 1) {
		scaled /= 1024;
		unit += 1;
	}
	const suffix = ['', 'K', 'M', 'G'][unit];
	return `${value < 0 ? '-' : ''}${formatScaled(scaled)}${suffix}`;
}

/** 命令数按整数显示，速率按每秒次数。 */
export function formatCount(value: number): string {
	return Number.isFinite(value) ? String(Math.round(value)) : '—';
}

/** CDC 停止位枚举（0=1、1=1.5、2=2）。 */
const STOP_BITS_TEXT: Readonly<Record<number, string>> = { 0: '1', 1: '1.5', 2: '2' };

/** CDC 校验枚举（0=无、1=奇、2=偶、3=Mark、4=Space）。 */
const PARITY_TEXT: Readonly<Record<number, string>> = { 0: 'N', 1: 'O', 2: 'E', 3: 'M', 4: 'S' };

/** CDC 线编码显示（如 115200 8N1）；设备不上报（老固件）时返回「—」。 */
export function formatLineCoding(stats: Stats | null): string {
	if (stats === null || stats.uartBaud === null || stats.uartDataBits === null) return '—';
	const stop = STOP_BITS_TEXT[stats.uartStopBits ?? 0] ?? '?';
	const parity = PARITY_TEXT[stats.uartParity ?? 0] ?? '?';
	return `${stats.uartBaud} ${stats.uartDataBits}${parity}${stop}`;
}

/** 串口是否被主机打开（TinyUSB 的 DTR）；设备不上报时返回「—」。 */
export function formatUartOpen(stats: Stats | null): string {
	if (stats === null || stats.uartHostOpen === null) return '—';
	return stats.uartHostOpen !== 0 ? '已打开' : '未打开';
}

/** 对端往返时延：设备上报微秒，这里换算成毫秒（两位小数）。 */
export function formatRtt(us: number | null | undefined): string {
	if (us === null || us === undefined || !Number.isFinite(us) || us <= 0) return '—';
	return `${(us / 1000).toFixed(2)} ms`;
}

export function formatUptime(ms: number): string {
	if (!Number.isFinite(ms) || ms < 0) return '—';
	const totalSeconds = Math.floor(ms / 1000);
	const days = Math.floor(totalSeconds / 86400);
	const hours = Math.floor((totalSeconds % 86400) / 3600);
	const minutes = Math.floor((totalSeconds % 3600) / 60);
	const seconds = totalSeconds % 60;
	const pad = (value: number) => String(value).padStart(2, '0');
	if (days > 0) return `${days}d ${pad(hours)}h ${pad(minutes)}m`;
	if (hours > 0) return `${pad(hours)}h ${pad(minutes)}m ${pad(seconds)}s`;
	if (minutes > 0) return `${pad(minutes)}m ${pad(seconds)}s`;
	return `${seconds}s`;
}

function formatScaled(value: number): string {
	if (value === 0) return '0';
	if (value < 10) return value.toFixed(2);
	if (value < 100) return value.toFixed(1);
	return Math.round(value).toString();
}
