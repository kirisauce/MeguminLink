/**
 * WebUSB 可用性诊断：把「WebUSB 为什么不可用 / 为什么连不上」拆成可分辨的检查项，
 * 每一项给出当时的证据与可操作建议。
 *
 * 探测输入全部由调用方注入——本模块不碰 window / navigator，因此既能在 SSR 阶段
 * 安全导入，也能在宿主机上喂假环境跑用例。
 */

import { STANDALONE_HREF } from './standalone';

export type CheckState = 'pass' | 'fail' | 'unknown';

export type UsbCheckId =
	| 'secure-context'
	| 'usb-api'
	| 'permissions-policy'
	| 'granted-device'
	| 'device-open';

export interface UsbCheck {
	id: UsbCheckId;
	label: string;
	state: CheckState;
	/** 当时的证据或结论 */
	detail: string;
	/** 失败时的可操作建议（通过项为空数组） */
	advice: string[];
	/** 该检查项附带的可点击目标（例如单文件版本的保存入口） */
	link?: UsbAdviceLink;
}

/** 建议附带的可点击目标 */
export interface UsbAdviceLink {
	label: string;
	href: string;
	download?: boolean;
}

export interface UsbEnvironment {
	isSecureContext: boolean;
	hasUsbApi: boolean;
	/** null = 该浏览器不提供 usb 权限策略查询 */
	usbPolicyAllowed: boolean | null;
	href: string;
	/** 已授权且 VID/PID 匹配的设备数；null = 未查询或查询失败 */
	grantedMatchingDevices: number | null;
	/** 最近一次打开设备的失败；null = 没有失败记录 */
	openFailure: { name: string; message: string } | null;
	/** 期望的设备名，用于文案 */
	deviceLabel: string;
}

export type DiagnosisStatus = 'ok' | 'blocked' | 'degraded';

export interface UsbDiagnosis {
	status: DiagnosisStatus;
	/** 一句话结论：取最靠前的失败项 */
	headline: string;
	checks: UsbCheck[];
	/** 失败项建议的汇总（去重后按检查顺序排列） */
	advice: string[];
	/** 失败项附带的链接（取最靠前的那个） */
	link?: UsbAdviceLink;
}

/** 主机名；file:// 这类没有主机名的来源返回 null，文案回退到完整 href */
function hostOf(href: string): string | null {
	try {
		const host = new URL(href).hostname;
		return host === '' ? null : host;
	} catch {
		return null;
	}
}

/** 打开设备失败的错误名 → 结论与建议 */
function describeOpenFailure(failure: { name: string; message: string }): {
	detail: string;
	advice: string[];
} {
	switch (failure.name) {
		case 'AccessDeniedError':
			return {
				detail: '设备被其它程序占用了',
				advice: ['你可以：', '关掉正在用这块板子的程序（pyOCD、OpenOCD、串口工具），再试一次'],
			};
		case 'NetworkError':
			return {
				detail: '设备打不开：驱动没装好，或者被别的程序占着',
				advice: [
					'你可以：',
					'Windows 上装一次 WinUSB 驱动（Zadig 或设备自带的 .inf），设备管理器里不要有黄色感叹号',
					'确认没有别的程序正在用这块板子',
				],
			};
		case 'NotFoundError':
			return { detail: '设备已经拔掉了', advice: ['你可以：', '插好后重试'] };
		case 'SecurityError':
			return {
				detail: '浏览器拦下了这次调用',
				advice: ['你可以：', '点「连接设备」按钮重试（浏览器要求由点击触发）'],
			};
		default:
			return { detail: `浏览器报错 ${failure.name}：${failure.message}`, advice: [] };
	}
}

const HEADLINES: Record<UsbCheckId, string> = {
	'secure-context': '当前地址不是 secure context，WebUSB 被禁用',
	'usb-api': '这个浏览器不支持 WebUSB',
	'permissions-policy': '页面策略禁止使用 WebUSB',
	'granted-device': '还没有授权过设备',
	'device-open': '设备打开失败',
};

export function diagnoseWebUsb(env: UsbEnvironment): UsbDiagnosis {
	const checks: UsbCheck[] = [];
	const host = hostOf(env.href);

	// 1) 安全上下文：浏览器只对 https 与 localhost 暴露 WebUSB。非安全上下文里
	//    navigator.usb 根本不存在——不是「被禁用」，而是接口没有被定义。
	if (env.isSecureContext) {
		checks.push({
			id: 'secure-context',
			label: 'secure context',
			state: 'pass',
			detail: `${host ?? env.href} 是 secure context`,
			advice: [],
		});
	} else {
		checks.push({
			id: 'secure-context',
			label: 'secure context',
			state: 'fail',
			detail: `${host ?? env.href} 不是 secure context，浏览器不暴露 WebUSB 接口`,
			advice: [
				'你可以：',
				'下载单文件HTML到本地使用',
				'通过 https 服务访问这个页面',
				'或者用 chrome://flags 将这个site临时标记为secure',
			],
			link: {
				label: '点这里下载单文件版本~',
				href: STANDALONE_HREF,
				download: true,
			},
		});
	}

	// 2) WebUSB 接口本身：只有安全上下文下的接口缺失才归因于浏览器或策略。
	if (env.hasUsbApi) {
		checks.push({
			id: 'usb-api',
			label: 'WebUSB 接口',
			state: 'pass',
			detail: 'navigator.usb 可用',
			advice: [],
		});
	} else {
		checks.push({
			id: 'usb-api',
			label: 'WebUSB 接口',
			state: env.isSecureContext ? 'fail' : 'unknown',
			detail: env.isSecureContext ? '地址没问题，但 navigator.usb 不存在' : '接口未暴露，先解决 secure context',
			advice: env.isSecureContext
				? [
						'你可以：',
						'用 Chrome 或 Edge 打开这个页面',
						'已经用了 Chrome/Edge 的话，去 chrome://policy 看看 WebUsbAllowed 是不是被禁用了',
					]
				: [],
		});
	}

	// 3) Permissions-Policy：嵌在 iframe 里且未 allow="usb" 时会被拦。
	if (env.usbPolicyAllowed === null) {
		checks.push({
			id: 'permissions-policy',
			label: 'Permissions-Policy',
			state: 'unknown',
			detail: '当前浏览器不提供 usb 权限策略查询',
			advice: [],
		});
	} else if (env.usbPolicyAllowed) {
		checks.push({
			id: 'permissions-policy',
			label: 'Permissions-Policy',
			state: 'pass',
			detail: 'usb 权限未被页面策略禁止',
			advice: [],
		});
	} else {
		checks.push({
			id: 'permissions-policy',
			label: 'Permissions-Policy',
			state: 'fail',
			detail: '页面策略禁止使用 usb',
			advice: [
				'你可以：',
				'给嵌入这个页面的 iframe 加上 allow="usb"',
				'或者直接在新标签页打开这个site',
			],
		});
	}

	// 4) 已授权设备：接口不可用时无从查起，标为未知而不是失败。
	if (!env.hasUsbApi) {
		checks.push({
			id: 'granted-device',
			label: '已授权设备',
			state: 'unknown',
			detail: '接口不可用，未检查已授权设备',
			advice: [],
		});
	} else if (env.grantedMatchingDevices === null) {
		checks.push({
			id: 'granted-device',
			label: '已授权设备',
			state: 'unknown',
			detail: '未能读取已授权设备列表',
			advice: [],
		});
	} else if (env.grantedMatchingDevices > 0) {
		checks.push({
			id: 'granted-device',
			label: '已授权设备',
			state: 'pass',
			detail: `已授权 ${env.grantedMatchingDevices} 台 ${env.deviceLabel}`,
			advice: [],
		});
	} else {
		checks.push({
			id: 'granted-device',
			label: '已授权设备',
			state: 'fail',
			detail: `还没有授权过 ${env.deviceLabel}`,
			advice: ['你可以：', '插上设备，点右上角「连接设备」，选中它'],
		});
	}

	// 5) 打开设备：失败原因按 DOMException.name 分流。
	if (env.openFailure === null) {
		checks.push({
			id: 'device-open',
			label: '打开设备',
			state: 'pass',
			detail: '最近一次打开请求没有报错',
			advice: [],
		});
	} else {
		const described = describeOpenFailure(env.openFailure);
		checks.push({
			id: 'device-open',
			label: '打开设备',
			state: 'fail',
			detail: described.detail,
			advice: described.advice,
		});
	}

	const failed = checks.filter((check) => check.state === 'fail');
	// 环境三项决定「能不能用」，设备两项只说明「为什么还没连上」。
	const blocking = failed.some(
		(check) => check.id === 'secure-context' || check.id === 'usb-api' || check.id === 'permissions-policy'
	);
	const status: DiagnosisStatus = blocking ? 'blocked' : failed.length > 0 ? 'degraded' : 'ok';

	const advice: string[] = [];
	for (const check of failed) {
		for (const line of check.advice) {
			if (!advice.includes(line)) advice.push(line);
		}
	}

	const headline =
		failed.length === 0
			? 'WebUSB 可用'
			: failed[0].id === 'granted-device' || failed[0].id === 'device-open'
				? failed[0].detail
				: HEADLINES[failed[0].id];

	const link = failed.find((check) => check.link !== undefined)?.link;

	return { status, headline, checks, advice, link };
}
