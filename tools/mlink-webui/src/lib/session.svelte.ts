/**
 * WebUSB 会话：授权设备、按固定间隔读取统计快照、处理拔插与页面可见性。
 *
 * 页面与 pyOCD/OpenOCD 并存的关键约定：本模块**从不**调用 claimInterface。
 * 统计请求走 EP0 的 device recipient 控制传输，不需要占用任何接口；
 * 一旦 claim 接口，Windows 上的 WinUSB 与 Linux 上的 libusb 都会与烧录器互斥。
 *
 * 轮询用「上次请求返回后再排下一次」的方式串行化，天然不会在设备变慢时堆积请求。
 */

import {
	CMD_CLEAR_PAIR,
	CMD_ENTER_PAIRING,
	CMD_EXIT_PAIRING,
	CMD_REBOOT,
	CMD_RESET,
	CMD_SET_MODE,
	PID,
	ProtocolError,
	STATUS_NAMES,
	STAT_FIELDS,
	USB_DRAFT_PENDING,
	USB_REJECT_TEXT,
	VID,
	applyUsbConfig,
	readConfig,
	readStats,
	readUsbConfig,
	runCommand,
	writeUsbConfig,
	type DeviceConfig,
	type Stats,
	type UsbCapability,
	type UsbConfig,
} from './protocol';
import { computeRates, type Rates } from './rate';

/** 页面在前台时的采样间隔。 */
const POLL_INTERVAL_MS = 200;
/** 页面切到后台时降频，避免被浏览器节流后产生无意义的密集唤醒。 */
const POLL_INTERVAL_HIDDEN_MS = 1000;
/** 重新枚举后等待设备接回的时长：超时则提示用户手动重新连接。 */
const RECONNECT_TIMEOUT_MS = 5000;

/** 曲线滚动窗口长度。 */
export const WINDOW_MS = 60_000;

/**
 * 曲线上的一个采样点。t 是主机时间（performance.now 毫秒），
 * rates 为 null 表示此处数据不连续（设备重启、断开重连、长时间挂起），
 * 曲线应当在该点断开而不是把两侧连成一条假直线。
 */
export type Sample = { t: number; rates: Rates | null };

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

/** 控制命令失败的行内文案：设备返回的错误状态优先于传输层描述。 */
function describeCommandError(error: unknown): string {
	if (error instanceof ProtocolError && error.status !== null) {
		switch (error.status) {
			case 0x01:
				return '固件不支持该命令';
			case 0x05:
				return '设备忙，请稍后重试';
			default:
				return `设备返回 ${STATUS_NAMES[error.status] ?? `0x${error.status.toString(16)}`}`;
		}
	}
	return `传输失败：${describeError(error)}`;
}

export class MlinkSession {
	state = $state<ConnectionState>('disconnected');
	stats = $state<Stats | null>(null);
	/** 最近两个相邻快照之间的每秒增量；不足两帧或间隔不连续时为 null。 */
	rates = $state<Rates | null>(null);
	/** 滚动窗口内的采样点，供曲线使用。 */
	history = $state<Sample[]>([]);
	/** 最近一次失败的说明；成功读回一帧后清空。 */
	error = $state<string | null>(null);
	device = $state<USBDevice | null>(null);
	/** 当前采样间隔，切前后台时会变。 */
	intervalMs = $state(POLL_INTERVAL_MS);
	/** 设备身份与配置项；读不到（老固件没有该命令、或未连接）时为 null。 */
	config = $state<DeviceConfig | null>(null);
	/** USB 能力（WebUSB / WinUSB / landing page）；读不到时为 null。 */
	usbConfig = $state<UsbConfig | null>(null);
	/** 控制命令在途：期间界面禁用全部操作按钮，避免重复提交。 */
	commandPending = $state(false);
	/** 最近一次控制命令的失败说明；下一条命令开始时清空。 */
	commandError = $state<string | null>(null);

	#timer: ReturnType<typeof setTimeout> | null = null;
	#reconnectTimer: ReturnType<typeof setTimeout> | null = null;
	#seq = 0;
	#listenersBound = false;
	#destroyed = false;
	/** 刚应用完 USB 能力、设备正在重新枚举：此间的断开与轮询失败都是预期的 */
	#expectingReconnect = false;

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
		this.rates = null;
		this.history = [];
		this.error = null;
		this.config = null;
		this.usbConfig = null;
		this.commandError = null;
		this.#expectingReconnect = false;
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
		this.#expectingReconnect = false;
		if (this.#reconnectTimer !== null) {
			clearTimeout(this.#reconnectTimer);
			this.#reconnectTimer = null;
		}
		this.state = 'streaming';
		void this.refreshConfig();
		void this.refreshUsbConfig();
		this.#schedule(0);
	}

	/**
	 * 读一次设备身份与配置项（连接后自动读、抽屉打开时刷新）。
	 *
	 * 读不到就保持 null、面板显示「—」：设备身份是附加信息，任何失败都
	 * 不该把会话推进 error 状态——链路问题由轮询统一发现。
	 */
	async refreshConfig(): Promise<void> {
		const device = this.device;
		if (device === null) return;
		try {
			this.config = await readConfig(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;
		} catch {
			this.config = null;
		}
	}

	/**
	 * 读一次 USB 能力（连接后自动读、抽屉打开时刷新）。
	 *
	 * 与 refreshConfig 同样的容错：读不到就保持 null，链路由轮询统一发现。
	 */
	async refreshUsbConfig(): Promise<void> {
		const device = this.device;
		if (device === null) return;
		try {
			this.usbConfig = await readUsbConfig(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;
		} catch {
			this.usbConfig = null;
		}
	}

	/**
	 * 保存 USB 能力：提交草稿 → 读校验结果 → 应用（设备随后重新枚举 USB）。
	 *
	 * 设备在应用后立刻软断开，这里不等待它回来：主动停掉轮询、清空快照并把
	 * 状态置为 connecting，等 connect 事件触发 restore() 接回后重读配置。
	 *
	 * @return true=已应用（设备开始重新枚举）；false=被拒或传输失败（见 commandError）
	 */
	async saveUsbConfig(capability: UsbCapability): Promise<boolean> {
		const device = this.device;
		if (device === null || this.commandPending) return false;
		this.commandPending = true;
		this.commandError = null;
		try {
			await writeUsbConfig(device, this.#seq, capability);
			this.#seq = (this.#seq + 1) & 0xff;

			const status = await readUsbConfig(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;
			if (status.draft !== USB_DRAFT_PENDING) {
				const reason = status.draftReason === null ? null : USB_REJECT_TEXT[status.draftReason];
				this.commandError = reason ?? '设备没有接受这份配置';
				return false;
			}

			await applyUsbConfig(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;

			this.#expectingReconnect = true;
			this.#cancel();
			this.device = null;
			this.stats = null;
			this.rates = null;
			this.config = null;
			this.state = 'connecting';
			this.error = '设备正在重新枚举 USB…';
			this.#scheduleReconnectTimeout();
			return true;
		} catch (error) {
			this.commandError = describeCommandError(error);
			return false;
		} finally {
			this.commandPending = false;
		}
	}

	async setMode(mode: number): Promise<void> {
		await this.#sendCommand(CMD_SET_MODE, mode);
	}

	async enterPairing(): Promise<void> {
		await this.#sendCommand(CMD_ENTER_PAIRING);
	}

	async exitPairing(): Promise<void> {
		await this.#sendCommand(CMD_EXIT_PAIRING);
	}

	async clearPair(): Promise<void> {
		await this.#sendCommand(CMD_CLEAR_PAIR);
	}

	async reset(): Promise<void> {
		await this.#sendCommand(CMD_RESET);
	}

	async reboot(): Promise<void> {
		await this.#sendCommand(CMD_REBOOT);
	}

	/**
	 * 下发一条控制命令。
	 *
	 * ok 只代表设备已接受投递：动作由设备主循环执行，结果通过轮询的模式与
	 * 配对状态体现；失败一律转成 commandError 交给界面行内显示。未连接或
	 * 已有命令在途时直接返回（按钮同时是禁用的，这里是兜底）。
	 */
	async #sendCommand(cmd: number, arg = 0): Promise<void> {
		const device = this.device;
		if (device === null || this.commandPending) return;
		this.commandPending = true;
		this.commandError = null;
		try {
			await runCommand(device, cmd, this.#seq, arg);
			this.#seq = (this.#seq + 1) & 0xff;
		} catch (error) {
			this.commandError = describeCommandError(error);
		} finally {
			this.commandPending = false;
		}
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
		if (device === null || this.#expectingReconnect) return;
		try {
			const stats = await readStats(device, this.#seq);
			this.#seq = (this.#seq + 1) & 0xff;
			const previous = this.stats;
			this.rates = previous === null ? null : computeRates(previous, stats);
			this.stats = stats;
			// 配对或模式跃迁说明配置也可能变了（建立/清除配对会改写 peer_mac，切模式
			// 会改写持久化模式），此时重读一次 GetConfig；config 不跟随每帧轮询。
			if (previous !== null && (previous.pair !== stats.pair || previous.mode !== stats.mode)) {
				void this.refreshConfig();
			}
			this.error = null;
			if (this.state !== 'streaming') this.state = 'streaming';
			this.#appendSample();
		} catch (error) {
			if (this.#expectingReconnect) return; // 重枚举中的断开是预期的，交给 connect 事件接回
			this.#fail(describeError(error));
			this.device = null;
			this.stats = null;
			this.rates = null;
			return;
		}
		this.#schedule();
	}

	/** 追加一个采样点并丢弃滚出窗口的旧点。 */
	#appendSample(): void {
		const now = performance.now();
		const cutoff = now - WINDOW_MS;
		const kept = this.history.filter((sample) => sample.t >= cutoff);
		kept.push({ t: now, rates: this.rates });
		this.history = kept;
	}

	#cancel(): void {
		if (this.#timer !== null) {
			clearTimeout(this.#timer);
			this.#timer = null;
		}
		if (this.#reconnectTimer !== null) {
			clearTimeout(this.#reconnectTimer);
			this.#reconnectTimer = null;
		}
	}

	/**
	 * 等待重新枚举的设备接回：超时说明 connect 事件没等到（浏览器未自动接回），
	 * 直接提示用户重新连接，避免页面永远停在"连接中"。
	 */
	#scheduleReconnectTimeout(): void {
		if (this.#reconnectTimer !== null) clearTimeout(this.#reconnectTimer);
		this.#reconnectTimer = setTimeout(() => {
			this.#reconnectTimer = null;
			if (!this.#expectingReconnect) return;
			this.#expectingReconnect = false;
			this.state = 'disconnected';
			this.error = '设备重新枚举后未能自动接回，请重新连接设备';
			void this.restore();
		}, RECONNECT_TIMEOUT_MS);
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
			this.rates = null;
			this.config = null;
			this.commandError = null;
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
