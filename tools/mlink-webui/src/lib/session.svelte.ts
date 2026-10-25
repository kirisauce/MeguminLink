/**
 * WebUSB 会话：授权设备、按固定间隔读取统计快照、处理拔插与页面可见性。
 *
 * 页面与 pyOCD/OpenOCD 并存的关键约定：本模块**从不**调用 claimInterface。
 * 统计请求走 EP0 的 device recipient 控制传输，不需要占用任何接口；
 * 一旦 claim 接口，Windows 上的 WinUSB 与 Linux 上的 libusb 都会与烧录器互斥。
 *
 * 轮询用「上次请求返回后再排下一次」的方式串行化，天然不会在设备变慢时堆积请求。
 *
 * 目标设备按序列号锁定：切换器选中的那台会被记住，拔插、重新枚举与页面刷新都接回
 * 同一台，而不是「已授权列表里的第一台」。失败由 session_recovery 归因，能自愈的
 * 按退避继续尝试、期间停在「等待设备」；每次尝试都重新解析设备对象，因为重新枚举
 * 之后是新的 USBDevice，旧句柄已经作废。
 */

import { deviceEntries, deviceKey, isMlinkDevice, pickKey, type DeviceEntry } from './devices';
import {
	CMD_CLEAR_PAIR,
	CMD_ENTER_PAIRING,
	CMD_ENTER_UPDATE,
	CMD_EXIT_PAIRING,
	CMD_REBOOT,
	CMD_RESET,
	CMD_SET_MODE,
	CMD_SET_LOSS,
	CMD_SET_UART_RELIABLE,
	CMD_SET_WIFI_RATE,
	MODE_WIRELESS,
	type LinkConfigPatch,
	PAIR_PAIRED,
	PID,
	ProtocolError,
	STATUS_NAMES,
	STAT_FIELDS,
	USB_DRAFT_PENDING,
	USB_REJECT_TEXT,
	VID,
	applyUsbConfig,
	pingPeer,
	readConfig,
	writeLinkConfig,
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
import { GONE_MESSAGE, backoffMs, planRecovery, type FailurePhase } from './session_recovery';
import { diagnoseWebUsb, type UsbDiagnosis } from './usb_diagnosis';

/** 页面在前台时的采样间隔。 */
const POLL_INTERVAL_MS = 200;
/** 页面切到后台时降频，避免被浏览器节流后产生无意义的密集唤醒。 */
const POLL_INTERVAL_HIDDEN_MS = 1000;
/**
 * 重新枚举的提示升级时长：这么久还没接回就说明设备回来得慢，文案改成「仍在重试」，
 * 会话本身继续按退避尝试，不需要用户操作。
 */
const RECONNECT_NOTICE_MS = 5000;
/** 应用 USB 配置后的第一次接回尝试：设备软断开到重新枚举完成通常在几百毫秒内。 */
const REENUMERATE_RETRY_MS = 600;
/** 设备正在重新枚举 USB：此间轮询失败与断开都是预期的。 */
const REENUMERATING_MESSAGE = '设备正在重新枚举 USB…';
/**
 * 对端探测的界面兜底超时：设备端超时窗口（Kconfig，默认 500ms）加上一次轮询
 * 往返还看不到计数变化，就收起「探测中」状态。
 */
const PING_TIMEOUT_MS = 1500;
/**
 * 上位机定时探测的间隔：设备不自主发帧，对端在线/往返时延由浏览器按这个节拍
 * 驱动刷新（点击状态条是同一路径的立即触发）。
 */
const PING_INTERVAL_MS = 3000;
/** 记住上次监控的那台设备（值是设备身份，不含任何敏感信息）。 */
const STORAGE_KEY = 'mlink-webui.device';

/** 曲线滚动窗口长度。 */
export const WINDOW_MS = 60_000;

/**
 * 曲线上的一个采样点。t 是主机时间（performance.now 毫秒），
 * rates 为 null 表示此处数据不连续（设备重启、断开重连、长时间挂起），
 * 曲线应当在该点断开而不是把两侧连成一条假直线。
 */
export type Sample = { t: number; rates: Rates | null };

export type ConnectionState =
	| 'unsupported'
	| 'disconnected'
	| 'connecting'
	/** 目标设备暂时不可用但已排好重试：被别的程序占着、刚拔掉、正在重新枚举 */
	| 'waiting'
	| 'streaming'
	| 'error';

export const STATE_TEXT: Readonly<Record<ConnectionState, string>> = {
	unsupported: '浏览器不支持 WebUSB',
	disconnected: '未连接',
	connecting: '连接中',
	waiting: '等待设备',
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
	/** 最近一次的等待说明或失败说明；成功读回一帧后清空。 */
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
	/** 探测在途（等设备结算）：并发保护用，周期探测与手动探测都会置位。 */
	#pingInFlight = $state(false);
	/**
	 * 手动触发的探测在途：只有它让状态条显示「探测中…」。3 秒一次的周期探测
	 * 静默刷新，文案保持在最近一次结论上，避免每 3 秒闪一下。
	 */
	manualPingInFlight = $state(false);
	/** 本 origin 已授权且当前在线的目标设备，供顶栏切换器列出。 */
	devices = $state<DeviceEntry[]>([]);
	/** 用户选中的那台设备的身份；未选过为 null，等待接回期间保持不变。 */
	currentKey = $state<string | null>(null);
	/** WebUSB 可用性诊断；未在客户端采集前为 null */
	usbDiagnosis = $state<UsbDiagnosis | null>(null);

	#timer: ReturnType<typeof setTimeout> | null = null;
	/** 退避重试定时器：失败后由自己排下一次尝试，成功或用户断开时清掉。 */
	#retryTimer: ReturnType<typeof setTimeout> | null = null;
	/** 重新枚举的提示升级定时器。 */
	#noticeTimer: ReturnType<typeof setTimeout> | null = null;
	#pingTimer: ReturnType<typeof setTimeout> | null = null;
	/** 上一次定时探测的主机时刻（performance.now 毫秒）。 */
	#pingProbeAt = 0;
	#seq = 0;
	#listenersBound = false;
	#destroyed = false;
	/** 烧录这类独占设备的过程期间为 true：轮询、重试、归因全部让路（见 suspend） */
	#suspended = false;
	/** 连续失败次数：决定退避节奏，任何一次成功都归零。 */
	#attempt = 0;
	/**
	 * 设备代数：每次换设备、松开句柄、重新打开都自增。异步读回来的结果只在代数
	 * 未变时写进状态，切换设备后旧设备的迟到响应不会污染新设备的面板。
	 */
	#generation = 0;
	/** 面板里当前这份数据属于哪台设备：换设备才清曲线，同一台重连保留（由断点分段）。 */
	#shownKey: string | null = null;
	/** 顶栏最近显示过的名字：句柄不在（等待接回）时沿用，不让文案在序列号与名字之间跳。 */
	#shownName = $state<string | null>(null);
	/** 目标设备的条目（产品名与序列号）：等待接回时菜单要按它列出那一行。 */
	#shownEntry = $state<DeviceEntry | null>(null);
	/** 设备正在重新枚举 USB：此间的等待文案固定为重枚举说明。 */
	#reenumerating = false;
	/** 最近一次打开设备的失败（诊断用）；成功打开后清空 */
	#openFailure: { name: string; message: string } | null = null;
	/** 诊断采集的单调序号：只允许最新一次采集写回 */
	#diagnosisSeq = 0;

	constructor() {
		// 初始状态与浏览器能力无关，保证服务端预渲染与客户端首帧一致；
		// 能力探测推迟到 restore()/request()，避免 hydration 前后状态跳变。
		this.state = 'disconnected';
	}

	/**
	 * 重新采集环境并刷新诊断：回答「WebUSB 为什么不可用 / 为什么连不上」。
	 * 只在客户端调用——SSR 阶段没有 window / navigator。
	 */
	async refreshDiagnosis(): Promise<void> {
		if (typeof window === 'undefined' || typeof navigator === 'undefined') return;

		// getDevices() 是异步的：连接成功后的那次采集可能被更早发出的慢查询追上，
		// 只有序号仍属于自己的采集才写回结果。
		const seq = ++this.#diagnosisSeq;

		const hasUsbApi = navigator.usb !== undefined;

		// Permissions-Policy 的新旧 API 名字不同，两个都探一下；查不到记 null（未知），
		// 不能把"查不了"当成"被禁止"。
		const policyHost = document as Document & {
			permissionsPolicy?: { allowsFeature(feature: string): boolean };
			featurePolicy?: { allowsFeature(feature: string): boolean };
		};
		const policyApi = policyHost.permissionsPolicy ?? policyHost.featurePolicy;
		let policyAllowed: boolean | null = null;
		if (policyApi !== undefined && typeof policyApi.allowsFeature === 'function') {
			try {
				policyAllowed = policyApi.allowsFeature('usb');
			} catch {
				policyAllowed = null;
			}
		}

		let granted: number | null = null;
		if (hasUsbApi) {
			try {
				const devices = await navigator.usb.getDevices();
				granted = devices.filter((item) => isMlinkDevice(item)).length;
			} catch {
				granted = null;
			}
		}

		if (seq !== this.#diagnosisSeq) return;
		this.usbDiagnosis = diagnoseWebUsb({
			isSecureContext: window.isSecureContext,
			hasUsbApi,
			usbPolicyAllowed: policyAllowed,
			href: location.href,
			grantedMatchingDevices: granted,
			openFailure: this.#openFailure,
			deviceLabel: `MeguminLink（${VID.toString(16)}:${PID.toString(16)}）`,
		});
	}

	get supported(): boolean {
		return typeof navigator !== 'undefined' && navigator.usb !== undefined;
	}

	/**
	 * 目标设备的条目：等待接回时它已经不在授权列表里，菜单靠这份缓存仍然列出那一行，
	 * 点它等于立刻重试。没有目标时为 null。
	 */
	get currentEntry(): DeviceEntry | null {
		return this.#shownEntry;
	}

	get deviceLabel(): string {
		const device = this.device;
		if (device === null) return this.#shownName ?? this.currentKey ?? '—';

		// 设过名字就显示名字（重新枚举接回后随 usbConfig 刷新），否则维持序列号
		const name = this.usbConfig?.deviceName ?? '';
		return name === '' ? deviceKey(device) : name;
	}

	/** 顶栏「设备」的悬停说明：USB 描述符里的产品名与序列号，不受自定义名影响 */
	get deviceDetail(): string {
		const device = this.device;
		if (device === null) return '';
		return [device.productName ?? '', deviceKey(device)].filter((part) => part !== '').join(' · ');
	}

	/**
	 * 当前设备的 USB 串号（固件填的是芯片 MAC），没有设备或描述符里没给时为 null。
	 * 两种启动模式共用同一个串号，因此它是"进更新模式前后是不是同一台"的比对依据。
	 */
	get deviceSerial(): string | null {
		return this.device?.serialNumber ?? null;
	}

	/**
	 * 页面加载时静默接回记住的那台设备。
	 *
	 * 只有「记住的还在」或「没有任何记忆且恰好一台已授权」才自动接；记住的暂时不在时
	 * 进等待态持续寻找（插回来自动接上），没有记忆却有多台就停在未连接，交给用户从
	 * 顶栏切换器选 —— 猜错会让人对着另一块板子的数据看。
	 */
	async restore(): Promise<void> {
		void this.refreshDiagnosis();
		if (!this.supported) {
			this.state = 'unsupported';
			return;
		}
		this.#bindListeners();
		if (this.device !== null) return;

		let granted: USBDevice[];
		try {
			granted = await navigator.usb.getDevices();
		} catch (error) {
			this.#recover(error, 'open');
			return;
		}
		this.#updateDevices(granted);

		const targets = granted.filter((item) => isMlinkDevice(item));
		const remembered = this.#readStoredKey();
		const key = pickKey(deviceEntries(targets), remembered);
		if (key === null) {
			if (remembered !== null) {
				// 记住的那台暂时不在：留着目标按退避找它，插回来自动接上
				this.currentKey = remembered;
				this.#attempt = 0;
				this.#beginWaiting(GONE_MESSAGE);
				return;
			}
			this.state = 'disconnected';
			return;
		}

		const device = targets.find((item) => deviceKey(item) === key);
		if (device === undefined) return;
		this.currentKey = key;
		await this.#open(device);
	}

	/** 重新读取已授权设备列表，供切换器刷新（设备可能刚插上或被拔走）。 */
	async refreshDevices(): Promise<void> {
		if (!this.supported) return;
		try {
			this.#updateDevices(await navigator.usb.getDevices());
		} catch {
			// 列表读不到就保留上一份：切换器最多是陈旧的，不影响当前会话
		}
	}

	/**
	 * 切换到已授权列表里的某台设备。
	 *
	 * 已经是当前这台就什么都不做；当前这台正在等待接回时，点一下等于「立刻重试」，
	 * 不必等退避到点。
	 */
	async selectTarget(key: string): Promise<void> {
		await this.refreshDevices();
		if (key === this.currentKey) {
			if (this.device !== null) return;
			this.#clearSuspend();
			void this.#retry();
			return;
		}
		const device = await this.#resolveTarget(key);
		if (device === null) return;
		await this.#select(device);
	}

	/** 由用户手势触发：弹出设备选择器。取消选择不是错误。 */
	async request(): Promise<void> {
		if (!this.supported) {
			this.state = 'unsupported';
			void this.refreshDiagnosis();
			return;
		}
		this.#bindListeners();
		try {
			const device = await navigator.usb.requestDevice({
				filters: [{ vendorId: VID, productId: PID }],
			});
			await this.#select(device);
		} catch (error) {
			if (error instanceof DOMException && error.name === 'NotFoundError') return;
			this.#recover(error, 'open');
		}
	}

	/** 用户主动断开：停止重试，也不再记住这台设备。 */
	async close(): Promise<void> {
		this.#generation++;
		this.#cancel();
		this.#forget();
		const device = this.device;
		this.device = null;
		this.currentKey = null;
		this.#shownKey = null;
		this.#shownName = null;
		this.#shownEntry = null;
		this.#attempt = 0;
		this.#reenumerating = false;
		this.stats = null;
		this.rates = null;
		this.history = [];
		this.error = null;
		this.config = null;
		this.usbConfig = null;
		this.commandError = null;
		this.commandPending = false;
		this.state = this.supported ? 'disconnected' : 'unsupported';
		if (device?.opened === true) {
			try {
				await device.close();
			} catch {
				// 设备已经消失时 close 会失败，忽略。
			}
		}
		void this.refreshDevices();
	}

	destroy(): void {
		this.#destroyed = true;
		this.#cancel();
	}

	/** 换到另一台设备：关旧句柄、清空面板、记住新目标，然后打开。 */
	async #select(device: USBDevice): Promise<void> {
		const key = deviceKey(device);
		if (this.device !== null && key === this.currentKey) return;
		this.#clearSuspend();
		const previous = this.device;
		if (previous !== null) {
			this.#generation++;
			this.#cancel();
			try {
				if (previous.opened) await previous.close();
			} catch {
				// 旧设备已经拔走时 close 会失败，忽略。
			}
		}
		this.#shownKey = null;
		this.#shownName = null;
		this.#shownEntry = null;
		this.stats = null;
		this.rates = null;
		this.history = [];
		this.config = null;
		this.usbConfig = null;
		this.commandError = null;
		this.commandPending = false;
		this.#attempt = 0;
		this.#reenumerating = false;
		this.currentKey = key;
		this.#remember(key);
		await this.#open(device);
	}

	/**
	 * 打开（或重新打开）设备并开始轮询。
	 *
	 * @param silent 退避重试路径传 true：不动状态条（停在「等待设备」），失败时同样
	 *               交给归因逻辑继续排下一次重试
	 */
	async #open(device: USBDevice, silent = false): Promise<void> {
		const generation = ++this.#generation;
		this.#cancelPoll();
		this.#cancelRetry();
		if (!silent) this.state = 'connecting';
		try {
			if (!device.opened) await device.open();
			if (device.configuration === null) await device.selectConfiguration(1);
		} catch (error) {
			if (generation !== this.#generation) return;
			this.#openFailure =
				error instanceof DOMException
					? { name: error.name, message: error.message }
					: { name: 'Error', message: describeError(error) };
			void this.refreshDiagnosis();
			this.#recover(error, 'open');
			return;
		}
		if (generation !== this.#generation) return;

		this.#openFailure = null;
		const key = deviceKey(device);
		if (this.#shownKey !== key) {
			// 换了设备：曲线重新开始，不把另一台的采样留在图上
			this.stats = null;
			this.rates = null;
			this.history = [];
		}
		this.#shownKey = key;
		this.#shownName = key;
		this.#shownEntry = this.devices.find((item) => item.key === key) ?? null;
		this.currentKey = key;
		this.#remember(key);
		this.#attempt = 0;
		this.#reenumerating = false;
		this.#cancelNotice();
		this.device = device;
		this.#seq = 0;
		this.#pingProbeAt = 0; // 接上设备即尽快探一次，不必等满一个间隔
		this.commandPending = false;
		this.error = null;
		this.state = 'streaming';
		void this.refreshDevices();
		void this.refreshConfig();
		void this.refreshUsbConfig();
		// 设备已授权并接上，「已授权设备」这项的结论变了，重新采集一次
		void this.refreshDiagnosis();
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
		const generation = this.#generation;
		try {
			const config = await readConfig(device, this.#seq);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
			this.config = config;
		} catch {
			if (generation !== this.#generation) return;
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
		const generation = this.#generation;
		try {
			const usbConfig = await readUsbConfig(device, this.#seq);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
			this.usbConfig = usbConfig;
			this.#shownName = usbConfig.deviceName === '' ? deviceKey(device) : usbConfig.deviceName;
		} catch {
			if (generation !== this.#generation) return;
			this.usbConfig = null;
		}
	}

	/**
	 * 保存 USB 能力：提交草稿 → 读校验结果 → 应用（设备随后重新枚举 USB）。
	 *
	 * 设备在应用后立刻软断开，这里不等待它回来：主动停掉轮询、清空快照并把
	 * 状态置为等待，接回由按序列号的重试链完成，接回后重读配置。
	 *
	 * @return true=已应用（设备开始重新枚举）；false=被拒或传输失败（见 commandError）
	 */
	async saveUsbConfig(capability: UsbCapability): Promise<boolean> {
		const device = this.device;
		if (device === null || this.commandPending) return false;
		const generation = this.#generation;
		this.commandPending = true;
		this.commandError = null;
		try {
			await writeUsbConfig(device, this.#seq, capability);
			if (generation !== this.#generation) return false;
			this.#seq = (this.#seq + 1) & 0xff;

			const status = await readUsbConfig(device, this.#seq);
			if (generation !== this.#generation) return false;
			this.#seq = (this.#seq + 1) & 0xff;
			if (status.draft !== USB_DRAFT_PENDING) {
				const reason = status.draftReason === null ? null : USB_REJECT_TEXT[status.draftReason];
				this.commandError = reason ?? '设备没有接受这份配置';
				return false;
			}

			await applyUsbConfig(device, this.#seq);
			if (generation !== this.#generation) return false;
			this.#seq = (this.#seq + 1) & 0xff;

			this.#generation++;
			this.#cancelPoll();
			this.#cancelRetry();
			this.device = null;
			this.stats = null;
			this.rates = null;
			this.config = null;
			this.commandPending = false;
			this.#reenumerating = true;
			this.#attempt = 0;
			this.error = REENUMERATING_MESSAGE;
			this.state = 'waiting';
			this.#scheduleRetry(REENUMERATE_RETRY_MS);
			this.#scheduleNotice();
			return true;
		} catch (error) {
			if (generation !== this.#generation) return false;
			this.commandError = describeCommandError(error);
			return false;
		} finally {
			if (generation === this.#generation) this.commandPending = false;
		}
	}

	async setMode(mode: number): Promise<void> {
		await this.#sendCommand(CMD_SET_MODE, mode);
	}

	/**
	 * 设置 ESP-NOW 发送速率档位（wifi_phy_rate_t 原值）。
	 *
	 * 设备端把档位应用到本地单播 peer，并把同一条控制请求转发给对端：两端跑
	 * 同一档位，上行与下行的丢包率才可比；转发是尽力而为的，对端没跟上时
	 * 重发一次即可。
	 *
	 * 回显值就地更新：命令返回 ok 即表示档位已被接受（取值域在设备端校验过、
	 * 动作槽空闲），实际应用在设备主循环里，立刻重读配置可能读到应用前的旧值。
	 * 配置快照在抽屉打开时会重新读，因此这里不额外对账。
	 */
	async setWifiRate(rate: number): Promise<void> {
		const generation = this.#generation;
		await this.#sendCommand(CMD_SET_WIFI_RATE, rate);
		if (generation !== this.#generation) return;
		if (this.commandError === null && this.config !== null) {
			this.config = { ...this.config, wifiRate: rate };
		}
	}

	/**
	 * 下发可靠传输参数（SetLinkConfig）。
	 *
	 * 与速率档位、丢包率不同，这里一次提交整组改动：设备每应用一次就会重置可靠
	 * 传输会话（换新 epoch 让两端重新对齐序号并同步参数），逐项提交会把会话反复
	 * 打断。参数随即生效并落盘，对端由会话重置帧拿到同一份。
	 *
	 * 应用在设备主循环里做，所以写完要重读一次配置——读到的才是生效值与新代数。
	 */
	async setLinkConfig(patch: LinkConfigPatch): Promise<void> {
		const device = this.device;
		if (device === null || this.commandPending) return;
		const generation = this.#generation;
		this.commandPending = true;
		this.commandError = null;
		try {
			await writeLinkConfig(device, this.#seq, patch);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
			// 设备应用是异步的：给主循环一拍再去读，免得读到旧值
			await new Promise((resolve) => setTimeout(resolve, 120));
			if (generation !== this.#generation) return;
			const config = await readConfig(device, this.#seq);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
			this.config = config;
		} catch (error) {
			if (generation !== this.#generation) return;
			this.commandError = describeCommandError(error);
		} finally {
			if (generation === this.#generation) this.commandPending = false;
		}
	}

	async setLossPermille(permille: number): Promise<void> {
		const generation = this.#generation;
		await this.#sendCommand(CMD_SET_LOSS, permille);
		if (generation !== this.#generation) return;
		if (this.commandError === null && this.config !== null) {
			this.config = { ...this.config, lossPermille: permille };
		}
	}

	/**
	 * 设置串口出向是否走可靠传输。
	 *
	 * 设备端把同一条控制请求转发给对端：两端必须一致，只改一端会变成"下载走可靠、
	 * 回包走尽力而为"的半可靠链路。回显值就地更新——命令返回 ok 即表示已被接受，
	 * 实际应用在设备主循环，下一次轮询读到的是生效值。
	 */
	async setUartReliable(reliable: boolean): Promise<void> {
		const generation = this.#generation;
		await this.#sendCommand(CMD_SET_UART_RELIABLE, reliable ? 1 : 0);
		if (generation !== this.#generation) return;
		if (this.commandError === null && this.stats !== null) {
			this.stats = { ...this.stats, uartReliable: reliable ? 1 : 0 };
		}
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
	 * 让设备软复位进更新模式（随后由烧录流程接管串口）。
	 *
	 * 发完命令由调用方 suspend()：设备一进更新模式，vendor 请求就只剩 STALL，
	 * 让轮询继续跑只会刷出一串假故障。
	 */
	async enterUpdateMode(): Promise<void> {
		await this.#sendCommand(CMD_ENTER_UPDATE);
	}

	/**
	 * 挂起：把设备让给调用方独占（网页烧录）。
	 *
	 * 更新模式对 vendor 请求一律 STALL，轮询与重试会把它当成故障反复刷错误提示；
	 * 而且设备接下来要重新枚举，旧句柄必然作废。挂起期间本对象不碰设备，只把状态
	 * 停在等待态、提示条写上一句人话；恢复（resume，或用户显式重连）后照常按序列号接回来。
	 */
	suspend(message: string): void {
		if (this.#suspended) return;
		this.#suspended = true;
		this.#generation++;
		this.#cancel();
		this.#reenumerating = false;
		this.device = null;
		this.stats = null;
		this.rates = null;
		this.config = null;
		this.commandError = null;
		this.#attempt = 0;
		this.error = message;
		if (this.state !== 'unsupported') this.state = 'waiting';
	}

	/** 恢复：立刻试一次接回；设备还没回来的话照常按退避重试。 */
	resume(): void {
		if (!this.#suspended) return;
		this.#clearSuspend();
		if (this.#destroyed || this.currentKey === null) return;
		void this.#retry();
	}

	/**
	 * 退出挂起态（不触发接回，接回由调用方决定）。
	 *
	 * 挂起只是"设备暂时归烧录或更新模式独占"，不是禁止用户接手：硬复位回来之后
	 * 从顶栏「连接设备」或设备菜单显式重连，就该照常接上。
	 */
	#clearSuspend(): void {
		this.#suspended = false;
		this.#attempt = 0;
	}

	/**
	 * 立即探测已配对对端（点状态条的「对端」项）。
	 *
	 * 与周期探测共用同一条下发路径，区别只在界面：只有手动探测会显示「探测中…」。
	 * 若此刻已有一轮探测在途（周期探测刚发出），不再叠加请求，直接等它结算——
	 * 「现在就要个结果」在同一轮里同样得到满足。
	 */
	async ping(): Promise<void> {
		if (this.device === null || this.commandPending) return;
		const generation = this.#generation;
		this.commandError = null;
		this.#pingProbeAt = performance.now(); // 点击也算一次周期节拍，避免紧接着又发一轮
		this.manualPingInFlight = true;
		if (this.#pingInFlight) return;
		this.commandPending = true;
		try {
			await this.#probePing(true);
		} finally {
			if (generation === this.#generation) this.commandPending = false;
		}
	}

	/**
	 * 下发一次探测命令并等轮询结算。
	 *
	 * 设备端异步执行：命令只投递，协议层发出 Ping 并在收到 Pong 后刷新平滑
	 * 往返。本次轮询之后的快照已含新的 peerOnline / pingRttUs，下一拍轮询
	 * 收起在途状态。
	 *
	 * @param report 失败时是否写 commandError：定时探测静默跳过，不每 3 秒刷一条报错
	 */
	async #probePing(report: boolean): Promise<void> {
		const device = this.device;
		if (device === null) return;
		const generation = this.#generation;
		this.#pingInFlight = true;
		this.#schedulePingTimeout();
		try {
			await pingPeer(device, this.#seq);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
		} catch (error) {
			if (generation !== this.#generation) return;
			if (report) this.commandError = describeCommandError(error);
			this.#finishPing();
		}
	}

	/**
	 * 上位机定时探测：设备端不自主发帧，对端在线状态由这里按固定间隔驱动刷新。
	 *
	 * 只在监控中、无线且已配对、设备上报了探测能力（peerOnline 非 null，老固件不支持）
	 * 时触发；命令在途或上一次探测未结算则跳过本轮，等下一个间隔。
	 */
	#schedulePingProbe(): void {
		const stats = this.stats;
		if (stats === null || stats.mode !== MODE_WIRELESS || stats.pair !== PAIR_PAIRED) return;
		if (stats.peerOnline === null) return;
		if (this.commandPending || this.#pingInFlight) return;
		const now = performance.now();
		if (now - this.#pingProbeAt < PING_INTERVAL_MS) return;
		this.#pingProbeAt = now;
		void this.#probePing(false);
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
		const generation = this.#generation;
		this.commandPending = true;
		this.commandError = null;
		try {
			await runCommand(device, cmd, this.#seq, arg);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
		} catch (error) {
			if (generation !== this.#generation) return;
			this.commandError = describeCommandError(error);
		} finally {
			if (generation === this.#generation) this.commandPending = false;
		}
	}

	#schedule(delay = this.intervalMs): void {
		if (this.#timer !== null || this.#destroyed || this.#suspended) return;
		this.#timer = setTimeout(() => {
			this.#timer = null;
			void this.#poll();
		}, delay);
	}

	async #poll(): Promise<void> {
		const device = this.device;
		if (device === null || this.#suspended) return;
		const generation = this.#generation;
		try {
			const stats = await readStats(device, this.#seq);
			if (generation !== this.#generation) return;
			this.#seq = (this.#seq + 1) & 0xff;
			const previous = this.stats;
			this.rates = previous === null ? null : computeRates(previous, stats);
			this.stats = stats;
			// 配对或模式跃迁说明配置也可能变了（建立/清除配对会改写 peer_mac，切模式
			// 会改写持久化模式），此时重读一次 GetConfig；config 不跟随每帧轮询。
			if (previous !== null && (previous.pair !== stats.pair || previous.mode !== stats.mode)) {
				void this.refreshConfig();
			}
			// 探测在途：本轮快照已包含设备结算后的 peerOnline / pingRttUs
			if (this.#pingInFlight) this.#finishPing();
			// 定时探测由上位机发起，节拍与轮询同源（见 #schedulePingProbe）
			this.#schedulePingProbe();
			this.error = null;
			this.#attempt = 0;
			if (this.state !== 'streaming') this.state = 'streaming';
			this.#appendSample();
		} catch (error) {
			if (generation !== this.#generation) return;
			this.stats = null;
			this.rates = null;
			this.#recover(error, 'transfer');
			return;
		}
		this.#schedule();
	}

	/** 本次探测已结算（或界面兜底超时）：收起在途状态 */
	#finishPing(): void {
		this.#pingInFlight = false;
		this.manualPingInFlight = false;
		if (this.#pingTimer !== null) {
			clearTimeout(this.#pingTimer);
			this.#pingTimer = null;
		}
	}

	/**
	 * 探测的界面兜底超时：一次轮询往返还没收起状态就放弃。老固件不支持该命令
	 * 时命令本身已失败，这里只负责不让界面永远停在「探测中」。
	 */
	#schedulePingTimeout(): void {
		if (this.#pingTimer !== null) clearTimeout(this.#pingTimer);
		this.#pingTimer = setTimeout(() => {
			this.#pingTimer = null;
			this.#finishPing();
		}, PING_TIMEOUT_MS);
	}

	/** 追加一个采样点并丢弃滚出窗口的旧点。 */
	#appendSample(): void {
		const now = performance.now();
		const cutoff = now - WINDOW_MS;
		const kept = this.history.filter((sample) => sample.t >= cutoff);
		kept.push({ t: now, rates: this.rates });
		this.history = kept;
	}

	/** 停掉轮询与探测计时：不动等待态自己的重试与提示计时器。 */
	#cancelPoll(): void {
		if (this.#timer !== null) {
			clearTimeout(this.#timer);
			this.#timer = null;
		}
		this.#finishPing();
	}

	#cancelRetry(): void {
		if (this.#retryTimer !== null) {
			clearTimeout(this.#retryTimer);
			this.#retryTimer = null;
		}
	}

	#cancelNotice(): void {
		if (this.#noticeTimer !== null) {
			clearTimeout(this.#noticeTimer);
			this.#noticeTimer = null;
		}
	}

	/** 会话停下时清掉全部计时器（轮询、重试、提示升级、探测兜底）。 */
	#cancel(): void {
		this.#cancelPoll();
		this.#cancelRetry();
		this.#cancelNotice();
	}

	/** 重新枚举后还没接回：只升级文案，重试继续。 */
	#scheduleNotice(): void {
		this.#cancelNotice();
		this.#noticeTimer = setTimeout(() => {
			this.#noticeTimer = null;
			if (!this.#reenumerating || this.state !== 'waiting') return;
			this.error = '设备重新枚举后还没接回，仍在重试~';
		}, RECONNECT_NOTICE_MS);
	}

	/**
	 * 失败归因与处置：区分「被别的程序占着」「驱动没就绪」「设备已经走了」「要用户动手」，
	 * 能自愈的进等待态并排下一次退避重试，只有不能自愈的才停下并提示。
	 *
	 * 重试期间保住 device 句柄（句柄还在就原地重开），句柄已经作废时按序列号重新解析，
	 * 因此重新枚举后的新对象也能接上。
	 */
	#recover(error: unknown, phase: FailurePhase): void {
		// 挂起期间的失败是调用方自己的事（例如更新模式的 STALL），页面不许插手
		if (this.#suspended) return;
		const plan = planRecovery({
			phase,
			errorName: error instanceof DOMException ? error.name : null,
			attempt: this.#attempt + 1,
		});
		if (plan.retryAfterMs === null) {
			this.#generation++;
			this.#cancel();
			this.#reenumerating = false;
			this.device = null;
			this.stats = null;
			this.rates = null;
			this.config = null;
			this.commandError = null;
			this.error = plan.message;
			if (this.state !== 'unsupported') this.state = 'error';
			return;
		}
		this.#beginWaiting(this.#reenumerating ? REENUMERATING_MESSAGE : plan.message);
	}

	/**
	 * 进入等待态并排下一次重试。
	 *
	 * @param message 本次要显示的等待说明；重试本身失败时不换新文案，保留上一句更有
	 *                信息量的说明
	 */
	#beginWaiting(message?: string): void {
		this.#attempt++;
		this.state = 'waiting';
		if (message !== undefined) this.error = message;
		this.#scheduleRetry(backoffMs(this.#attempt));
	}

	#scheduleRetry(delay: number): void {
		if (this.#retryTimer !== null || this.#destroyed || this.#suspended) return;
		this.#retryTimer = setTimeout(() => {
			this.#retryTimer = null;
			void this.#retry();
		}, delay);
	}

	/** 退避到点后重试：句柄还在就原地重开，句柄没了就按序列号重新解析。 */
	async #retry(): Promise<void> {
		if (this.#destroyed || this.#suspended) return;
		const key = this.currentKey;
		if (key === null) return;

		const device = this.device ?? (await this.#resolveTarget(key));
		if (this.#destroyed) return;
		if (device === null) {
			// 设备还没回来：保持等待，按退避再看一次
			this.#beginWaiting();
			return;
		}
		await this.#open(device, true);
	}

	/** 按身份从已授权列表里找设备；重新枚举后是新的对象，所以每次都重新取。 */
	async #resolveTarget(key: string): Promise<USBDevice | null> {
		let granted: USBDevice[];
		try {
			granted = await navigator.usb.getDevices();
		} catch {
			return null;
		}
		this.#updateDevices(granted);
		return granted.find((item) => isMlinkDevice(item) && deviceKey(item) === key) ?? null;
	}

	#updateDevices(granted: USBDevice[]): void {
		this.devices = deviceEntries(granted);
	}

	#readStoredKey(): string | null {
		try {
			return localStorage.getItem(STORAGE_KEY);
		} catch {
			return null;
		}
	}

	#remember(key: string): void {
		try {
			localStorage.setItem(STORAGE_KEY, key);
		} catch {
			// 无存储可用（隐私模式、file:// 被拒）时不记住，功能不受影响
		}
	}

	#forget(): void {
		try {
			localStorage.removeItem(STORAGE_KEY);
		} catch {
			// 同上：清不掉也不影响本次断开
		}
	}

	#bindListeners(): void {
		if (this.#listenersBound || typeof navigator === 'undefined') return;
		this.#listenersBound = true;

		navigator.usb.addEventListener('disconnect', (event) => {
			void this.refreshDevices();
			this.#onDeviceGone(event.device);
		});

		navigator.usb.addEventListener('connect', () => {
			void this.refreshDevices();
			// 目标设备回来了：不必等退避到点，立刻试一次
			if (this.device === null && this.currentKey !== null) {
				this.#attempt = 0;
				this.#cancelRetry();
				void this.#retry();
			}
		});

		document.addEventListener('visibilitychange', () => {
			this.intervalMs = document.hidden ? POLL_INTERVAL_HIDDEN_MS : POLL_INTERVAL_MS;
		});
	}

	/** 本会话的设备被拔掉：松开句柄、清空面板，按身份继续找它。 */
	#onDeviceGone(device: USBDevice): void {
		if (this.#suspended) return;
		// 按身份字段比对，而不是对象同一性：$state 会把设备对象包成 Proxy，事件里带的
		// 原始对象与状态里读到的不可能是同一个引用
		if (this.device === null || deviceKey(device) !== this.currentKey) return;
		this.#generation++;
		this.#cancelPoll();
		this.#cancelRetry();
		this.device = null;
		this.stats = null;
		this.rates = null;
		this.config = null;
		this.commandError = null;
		this.commandPending = false;
		this.#attempt = 0;
		if (this.currentKey === null) {
			this.state = 'disconnected';
			return;
		}
		this.#beginWaiting(GONE_MESSAGE);
	}
}

export { STAT_FIELDS, POLL_INTERVAL_MS };
