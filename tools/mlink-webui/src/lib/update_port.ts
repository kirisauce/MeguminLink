/**
 * 烧录通道的获取：设备切进更新模式后认出它，并把它的 CDC 串口拿到手。
 *
 * 这两件事都是浏览器侧的管道而不是升级协议本身，所以单独放这里：
 *
 * 1. **等设备真的换成更新模式**。两种启动模式的 VID:PID 与串号完全相同，产品名是
 *    唯一能观测到的模式信号；而串口选择器只列"此刻已连接"的口，早弹了会看不到目标。
 * 2. **取串口**。先看本 origin 授权过的口里有没有 VID/PID 匹配的（第二次起不再弹
 *    选择器），没有才 requestPort()。Web Serial 拿不到串口的串号，所以"选中的口是
 *    不是刚才那台"只能靠升级协议里 Info 自报的 MAC 来核对（见 update.ts）。
 */

import { UPDATE_BAUD, UPDATE_PID, UPDATE_VID, type UpdateStreams } from './update';

/** 更新模式的 USB 产品名（固件 update_mode.cc 里那份）。 */
export const UPDATE_PRODUCT_NAME = 'MeguminLink Update';

/** 串口选择器的过滤条件：只列本项目的设备。 */
export const UPDATE_PORT_FILTERS: SerialPortFilter[] = [{ usbVendorId: UPDATE_VID, usbProductId: UPDATE_PID }];

/** 等设备换描述符的上限：软复位 + 重新枚举 + CDC 起来，实测量级是百毫秒。 */
const ENTER_TIMEOUT_MS = 10000;
/** 设备从授权列表里消失后，再等它带着新描述符回来。 */
const REENUMERATE_MS = 1200;
const POLL_MS = 150;

/** 打开好的串口：字节流交给升级协议，close 交回浏览器。 */
export type UpdatePort = {
	streams: UpdateStreams;
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

async function openPort(port: SerialPort): Promise<UpdatePort> {
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

/**
 * 拿到更新模式的串口。
 *
 * 返回 null 表示用户在选择器里点了取消（不是错误，调用方回到待机即可）；打不开、
 * 浏览器不支持等情况抛 Error。
 */
export async function acquireUpdatePort(): Promise<UpdatePort | null> {
	if (typeof navigator === 'undefined' || navigator.serial === undefined) {
		throw new Error('这个浏览器没有 Web Serial，烧录需要 Chrome 或 Edge');
	}

	const granted = await navigator.serial.getPorts();
	let port: SerialPort | null = granted.find(isMlinkPort) ?? null;
	if (port === null) {
		try {
			port = await navigator.serial.requestPort({ filters: UPDATE_PORT_FILTERS });
		} catch (cause) {
			if (cause instanceof DOMException && cause.name === 'NotFoundError') return null;
			throw new Error(`打开串口选择器失败：${describe(cause)}`);
		}
	}
	return await openPort(port);
}
