/**
 * 授权设备清单：把 WebUSB 的设备对象整理成顶栏切换器能用的一行行条目。
 *
 * 设备身份取序列号：固件把 MAC 填进字符串描述符，同一块板子跨重新枚举保持不变，
 * 因此它是「接回上次那台」的唯一可靠依据。拿不到序列号（规范允许）时回退 VID:PID，
 * 此时多台同型设备无法区分，条目带 keyless 让界面把这件事说出来。
 *
 * 本模块不碰 window / navigator / USBDevice 实例，宿主上喂假设备对象即可跑用例。
 */

import { PID, VID } from './protocol';

/** 设备对象里本模块用得到的部分；WebUSB 的 USBDevice 结构上满足它 */
export interface DeviceIdentity {
	vendorId: number;
	productId: number;
	productName?: string | null;
	serialNumber?: string | null;
}

/** 切换器里的一行 */
export interface DeviceEntry {
	/** 稳定标识：序列号，拿不到时回退 VID:PID */
	key: string;
	/** 主标题：USB 产品名（设过自定义设备名时产品名里已经含它） */
	label: string;
	/** 副标题：序列号，或 VID:PID */
	detail: string;
	/** 没有序列号：多台同型设备无法区分 */
	keyless: boolean;
}

const FALLBACK_LABEL = 'MeguminLink';

function fallbackKey(device: DeviceIdentity): string {
	return `${device.vendorId.toString(16)}:${device.productId.toString(16)}`;
}

export function isMlinkDevice(device: DeviceIdentity): boolean {
	return device.vendorId === VID && device.productId === PID;
}

export function deviceKey(device: DeviceIdentity): string {
	const serial = device.serialNumber ?? '';
	return serial === '' ? fallbackKey(device) : serial;
}

/** 过滤出本项目的设备并整理成条目，顺序沿用 WebUSB 给的顺序 */
export function deviceEntries(devices: DeviceIdentity[]): DeviceEntry[] {
	const entries: DeviceEntry[] = [];
	for (const device of devices) {
		if (!isMlinkDevice(device)) continue;
		const serial = device.serialNumber ?? '';
		const label = device.productName ?? '';
		entries.push({
			key: deviceKey(device),
			label: label === '' ? FALLBACK_LABEL : label,
			detail: serial === '' ? fallbackKey(device) : serial,
			keyless: serial === '',
		});
	}
	return entries;
}

/**
 * 没有用户选择时该接哪一台。
 *
 * 记住的那台还在就接它；没有任何记忆而列表里只有一台时接它（保持「刷新即接回」的
 * 体验）；记住的已经不在、或没有记忆却有多台，返回 null —— 猜错会让用户对着另一块
 * 板子的数据看，这种时候宁可不连，交给顶栏切换器选。
 */
export function pickKey(entries: DeviceEntry[], remembered: string | null): string | null {
	if (remembered === null) return entries.length === 1 ? entries[0].key : null;
	return entries.some((entry) => entry.key === remembered) ? remembered : null;
}
