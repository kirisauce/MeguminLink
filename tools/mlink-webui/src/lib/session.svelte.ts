/**
 * WebUSB 会话：授权设备、按固定间隔读取统计快照、处理拔插与页面可见性。
 *
 * 页面与 pyOCD/OpenOCD 并存的关键约定：本模块**从不**调用 claimInterface。
 * 统计请求走 EP0 的 device recipient 控制传输，不需要占用任何接口；
 * 一旦 claim 接口，Windows 上的 WinUSB 与 Linux 上的 libusb 都会与烧录器互斥。
 *
 * 轮询用「上次请求返回后再排下一次」的方式串行化，天然不会在设备变慢时堆积请求。
 */

import { ProtocolError, STAT_FIELDS, VID, PID, readStats, type Stats } from './protocol';

/** 页面在前台时的采样间隔。 */
const POLL_INTERVAL_MS = 200;
/** 页面切到后台时降频，避免被浏览器节流后产生无意义的密集唤醒。 */
const POLL_INTERVAL_HIDDEN_MS = 1000;

export type ConnectionState = 'unsupported' | 'disconnected' | 'connecting' | 'streaming' | 'error';

export const STATE_TEXT: Readonly<Record<ConnectionState, string>> = {
	unsupported: '浏览器不支持 WebUSB',
	disconnected: '未连接',
	connecting: '连接中',
	streaming: '监控中',
	error: '已中断',
};

function describeError(error: unknown): string {
	if (error instanceof ProtocolError) return error.message;
	if (error instanceof DOMException) return `${error.name}: ${error.message}`;
	if (error instanceof Error) return error.message;
	return String(error);
}

export class MlinkSession {
	state = $state<ConnectionState>('disconnected');
	stats = $state<Stats | null>(null);
	/** 最近一次失败的说明；成功读回一帧后清空。 */
	error = $state<string | null>(null);
	device = $state<USBDevice | null>(null);
	/** 当前采样间隔，切前后台时会变。 */
	intervalMs = $state(POLL_INTERVAL_MS);

	#timer: ReturnType<typeof setTimeout> | null = null;
	#seq = 0;
	#listenersBound = false;
	#destroyed = false;

	constructor() {
		// 初始状态与浏览器能力无关，保证服务端预渲染与客户端首帧一致；
		// 能力探测推迟到 restore()/request()，避免 hydration 前后状态跳变。
		this.state = 'disconnected';
	}

	get supported(): boolean {
		return typeof navigator !== 'undefined' && navigator.usb !== undefined;
	}

	get deviceLabel(): string {
		const device = this.device;
		if (device === null) return '—';
		const serial = device.serialNumber;
		return serial ? `${serial}` : `${device.vendorId.toString(16)}:${device.productId.toString(16)}`;
	}

	/** 页面加载时尝试静默接回本 origin 已授权过的设备，省掉每次刷新都要重新选设备。 */
	async restore(): Promise<void> {
		if (!this.supported) {
			this.state = 'unsupported';
			return;
		}
		this.#bindListeners();
		if (this.device !== null) return;
		try {
			const granted = await navigator.usb.getDevices();
			const device = granted.find((item) => item.vendorId === VID && item.productId === PID);
			if (device !== undefined) await this.#open(device);
		} catch (error) {
			this.#fail(describeError(error));
		}
	}

	/** 由用户手势触发：弹出设备选择器。取消选择不是错误。 */
	async request(): Promise<void> {
		if (!this.supported) {
			this.state = 'unsupported';
			return;
		}
		this.#bindListeners();
		try {
			const device = await navigator.usb.requestDevice({
				filters: [{ vendorId: VID, productId: PID }],
			});
			await this.#open(device);
		} catch (error) {
			if (error instanceof DOMException && error.name === 'NotFoundError') return;
			this.#fail(describeError(error));
		}
	}

	async close(): Promise<void> {
		this.#cancel();
		const device = this.device;
		this.device = null;
		this.stats = null;
		this.error = null;
		this.state = this.supported ? 'disconnected' : 'unsupported';
		if (device?.opened === true) {
			try {
				await device.close();
			} catch {
				// 设备已经消失时 close 会失败，忽略。
			}
		}
	}

	destroy(): void {
		this.#destroyed = true;
		this.#cancel();
	}

	async #open(device: USBDevice): Promise<void> {
		this.state = 'connecting';
		this.error = null;
		try {
			if (!device.opened) await device.open();
			if (device.configuration === null) await device.selectConfiguration(1);
		} catch (error) {
			this.#fail(`打开设备失败：${describeError(error)}`);
			return;
		}
		this.device = device;
		this.#seq = 0;
		this.state = 'streaming';
		this.#schedule(0);
	}

	#schedule(delay = this.intervalMs): void {
		if (this.#timer !== null || this.#destroyed) return;
		this.#timer = setTimeout(() => {
			this.#timer = null;
			void this.#poll();
		}, delay);
	}

	async #poll(): Promise<void> {
		const device = this.device;
		if (device === null) return;
		try {
			const stats = await readStats(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;
			this.stats = stats;
			this.error = null;
			if (this.state !== 'streaming') this.state = 'streaming';
		} catch (error) {
			this.#fail(describeError(error));
			this.device = null;
			this.stats = null;
			return;
		}
		this.#schedule();
	}

	#cancel(): void {
		if (this.#timer !== null) {
			clearTimeout(this.#timer);
			this.#timer = null;
		}
	}

	#fail(message: string): void {
		this.#cancel();
		this.error = message;
		if (this.state !== 'unsupported') this.state = 'error';
	}

	#bindListeners(): void {
		if (this.#listenersBound || typeof navigator === 'undefined') return;
		this.#listenersBound = true;

		navigator.usb.addEventListener('disconnect', (event) => {
			if (event.device !== this.device) return;
			this.device = null;
			this.stats = null;
			this.#cancel();
			this.state = 'disconnected';
			this.error = '设备已断开';
		});

		navigator.usb.addEventListener('connect', () => {
			if (this.device === null) void this.restore();
		});

		document.addEventListener('visibilitychange', () => {
			this.intervalMs = document.hidden ? POLL_INTERVAL_HIDDEN_MS : POLL_INTERVAL_MS;
		});
	}
}

export { STAT_FIELDS, POLL_INTERVAL_MS };
