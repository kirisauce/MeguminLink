/**
 * 烧录通道的获取：设备切进更新模式后认出它，并把它的 CDC 串口拿到手。
 *
 * 这两件事都是浏览器侧的管道而不是升级协议本身，所以单独放这里：
 *
 * 1. **等设备真的换成更新模式**。两种启动模式的 VID:PID 与串号完全相同，产品名是
 *    唯一能观测到的模式信号；而串口选择器只列"此刻已连接"的口，早弹了会看不到目标。
 * 2. **取串口**。已授权的口里可能混着别的板子（两种启动模式共用 VID:PID，Web Serial
 *    又只给 VID/PID），所以每个候选口都握一次手：自报 MAC 对得上的才留下，都不行才
 *    弹选择器让主人自己挑。
 */

import { UPDATE_BAUD, UPDATE_PID, UPDATE_VID, UpdateError, UpdateSession, type UpdateStreams } from './update';

/** 更新模式的 USB 产品名（固件 update_mode.cc 里那份）。 */
export const UPDATE_PRODUCT_NAME = 'MeguminLink Update';

/** 串口选择器的过滤条件：只列本项目的设备。 */
export const UPDATE_PORT_FILTERS: SerialPortFilter[] = [{ usbVendorId: UPDATE_VID, usbProductId: UPDATE_PID }];

/** 等设备换描述符的上限：软复位 + 重新枚举 + CDC 起来，实测量级是百毫秒。 */
const ENTER_TIMEOUT_MS = 10000;
/** 设备从授权列表里消失后，再等它带着新描述符回来。 */
const REENUMERATE_MS = 1200;
const POLL_MS = 150;
/**
 * 已授权过的串口：等它重新出现。比固定等待准——设备刚报出更新模式时 tty 往往还没建好。
 */
const GRANTED_WAIT_MS = 1500;
/**
 * 从没用过的串口：选择器只列"此刻已连接"的口，弹早了会是空的。Web Serial 看不到未
 * 授权的口，没法主动探测它何时出现，所以这里只能再留一段落定时间。
 */
const CHOOSER_SETTLE_MS = 500;

/** 打开好的串口：字节流交给升级协议，close 交回浏览器。 */
type OpenedPort = {
	streams: UpdateStreams;
	close: () => Promise<void>;
};

/**
 * 取到的更新模式串口：身份已经握过手，session 可以直接往下烧。
 *
 * 目标未知时（页面看不到设备）只要求对面接得上话，自报 MAC 由界面交给主人核。
 */
export type VerifiedUpdatePort = {
	session: UpdateSession;
	close: () => Promise<void>;
};

/** 这个浏览器能不能做网页烧录：要有 Web Serial。页面本身还得在 secure context 里。 */
export function canFlashFromBrowser(): boolean {
	return typeof navigator !== 'undefined' && navigator.serial !== undefined;
}

function delay(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function describe(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

const sameSerial = (got: string | null | undefined, want: string): boolean =>
	(got ?? '').toLowerCase() === want.toLowerCase();

const isUpdateProduct = (name: string | null | undefined): boolean => (name ?? '').includes(UPDATE_PRODUCT_NAME);

const isMlinkPort = (port: SerialPort): boolean => {
	const info = port.getInfo();
	return info.usbVendorId === UPDATE_VID && info.usbProductId === UPDATE_PID;
};

/**
 * 等这台设备带着更新模式的描述符回来。
 *
 * 两条判据取先到的一条：产品名变成 MeguminLink Update（授权跨重新枚举保留时走这条），
 * 或者它从授权列表里消失后再过一小会儿（授权被重新枚举打断时走这条，多等的那一点是
 * 留给 CDC 枚举完）。返回 false 表示超时——设备多半没有真的进更新模式。
 */
export async function waitForUpdateMode(serial: string | null, timeoutMs = ENTER_TIMEOUT_MS): Promise<boolean> {
	if (typeof navigator === 'undefined' || navigator.usb === undefined) return false;
	const deadline = Date.now() + timeoutMs;
	let goneAt: number | null = null;

	for (;;) {
		let devices: USBDevice[] = [];
		try {
			devices = await navigator.usb.getDevices();
		} catch {
			// 查询失败按"还没看到"处理，下一轮再试
		}
		const mine = devices.filter((device) => serial === null || sameSerial(device.serialNumber, serial));
		if (mine.some((device) => isUpdateProduct(device.productName))) return true;

		if (mine.length === 0) {
			const now = Date.now();
			goneAt ??= now;
			if (now - goneAt >= REENUMERATE_MS) return true;
		} else {
			goneAt = null;
		}

		if (Date.now() >= deadline) return false;
		await delay(POLL_MS);
	}
}

async function openPort(port: SerialPort): Promise<OpenedPort> {
	try {
		await port.open({ baudRate: UPDATE_BAUD });
	} catch (cause) {
		throw new Error(`打不开更新模式的串口：${describe(cause)}`);
	}

	if (port.readable === null || port.writable === null) {
		await port.close().catch(() => undefined);
		throw new Error('串口没有可用的读写流（可能已被其它程序占用）');
	}

	return {
		streams: { readable: port.readable, writable: port.writable },
		close: async () => {
			// 刷完设备就复位了，端口常常已经不在：关闭失败不是错误
			await port.close().catch(() => undefined);
		},
	};
}

/** 握手结果：认下来了、接话的另有其人、还是根本没接话。 */
type ProbeResult =
	| { kind: 'ready'; port: VerifiedUpdatePort }
	| { kind: 'foreign'; mac: string }
	| { kind: 'silent'; reason: string };

/**
 * 打开一个候选口，问它一句"你是谁"。
 *
 * 认下来就把会话原样交出去——口在设备重新枚举前后未必还能再开一次，所以不关掉重开；
 * 认不下来就关干净，把原因交回去让调用方接着试。
 */
async function probePort(port: SerialPort, expectedMac: string | null): Promise<ProbeResult> {
	let opened: OpenedPort;
	try {
		opened = await openPort(port);
	} catch (cause) {
		return { kind: 'silent', reason: describe(cause) };
	}

	let session: UpdateSession;
	try {
		session = await UpdateSession.open(opened.streams);
	} catch (cause) {
		await opened.close();
		return { kind: 'silent', reason: describe(cause) };
	}

	if (expectedMac !== null && session.info.mac !== null && !sameSerial(session.info.mac, expectedMac)) {
		const mac = session.info.mac;
		await session.close();
		await opened.close();
		return { kind: 'foreign', mac };
	}

	return {
		kind: 'ready',
		port: {
			session,
			close: async () => {
				await session.close();
				await opened.close();
			},
		},
	};
}

/**
 * 一个候选口都没认下时的说法。
 *
 * "接话的是别人"和"没人接话"是两回事：前者多半是目标没进更新模式或选错了口，后者更像
 * 口被占着、设备刚走。两种都摊开写，主人一眼能对上现场。
 */
function describeMisses(expectedMac: string | null, foreign: string[], silent: number, reason: string | null): string {
	const parts: string[] = [];
	if (foreign.length > 0) {
		const who = foreign.join('、');
		parts.push(
			expectedMac === null ? `串口上应答的是 ${who}` : `串口上应答的是 ${who}，不是当前设备 ${expectedMac}`
		);
	}
	if (silent > 0) {
		parts.push(`${silent} 个串口上没有更新模式在应答${reason === null ? '' : `（${reason}）`}`);
	}
	if (parts.length === 0) parts.push('没有可用的串口');
	return `没能接上更新模式的设备：${parts.join('；')}。一个字节都没写。`;
}

/**
 * 拿到更新模式的串口，并且确认它就是目标那台。
 *
 * 授权过的口逐个握手（第二次起走这条，不弹选择器）；一个都认不下才弹选择器。返回 null
 * 表示主人在选择器里点了取消（不是错误，调用方回到待机即可）。
 */
export async function acquireUpdatePort(expectedMac: string | null): Promise<VerifiedUpdatePort | null> {
	if (typeof navigator === 'undefined' || navigator.serial === undefined) {
		throw new Error('这个浏览器没有 Web Serial，烧录需要 Chrome 或 Edge');
	}

	/** 这一轮看到的情况，最后拼进失败文案 */
	const foreign: string[] = [];
	let silent = 0;
	let reason: string | null = null;

	const tryPorts = async (candidates: SerialPort[]): Promise<VerifiedUpdatePort | null> => {
		for (const candidate of candidates) {
			const probed = await probePort(candidate, expectedMac);
			if (probed.kind === 'ready') return probed.port;
			if (probed.kind === 'foreign') {
				foreign.push(probed.mac);
			} else {
				silent += 1;
				reason ??= probed.reason;
			}
		}
		return null;
	};

	const deadline = Date.now() + GRANTED_WAIT_MS;
	let granted: SerialPort[] = [];
	for (;;) {
		granted = (await navigator.serial.getPorts()).filter(isMlinkPort);
		if (granted.length > 0 || Date.now() >= deadline) break;
		await delay(POLL_MS);
	}
	const known = await tryPorts(granted);
	if (known !== null) return known;

	await delay(CHOOSER_SETTLE_MS);
	let chosen: SerialPort;
	try {
		chosen = await navigator.serial.requestPort({ filters: UPDATE_PORT_FILTERS });
	} catch (cause) {
		if (cause instanceof DOMException && cause.name === 'NotFoundError') return null;
		throw new Error(`打开串口选择器失败：${describe(cause)}`);
	}
	const picked = await tryPorts([chosen]);
	if (picked !== null) return picked;

	throw new UpdateError(describeMisses(expectedMac, foreign, silent, reason));
}
