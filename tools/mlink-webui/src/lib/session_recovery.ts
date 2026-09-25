/**
 * 会话失败的归因与重试节奏：输入一次失败的现场，输出「算什么故障、等多久再试、界面显示哪句」。
 *
 * 浏览器只给得出 DOMException.name，归因全靠它分流：设备被烧录器之类的程序占着报
 * AccessDeniedError，Windows 上同一个 WinUSB 子设备的独占表现为传输阶段的 NetworkError，
 * 设备被拔掉报 NotFoundError，浏览器要求用户手势时报 SecurityError。重试节奏与文案不该
 * 散在会话状态机里，所以单独放这里——本模块不碰 window / navigator / USBDevice 实例，
 * 宿主上直接喂假输入跑用例。
 */

export type FailurePhase = 'open' | 'transfer';

export type FailureKind =
	/** 设备被别的程序占着：等它让出来就自己接回 */
	| 'busy'
	/** 驱动没就绪或句柄被打断：重开设备即可自愈 */
	| 'driver'
	/** 设备已经不在了：按慢节拍复查并重开，插回来自动接上 */
	| 'gone'
	/** 需要用户动作（浏览器要求手势）：重试没有意义 */
	| 'fatal';

export interface FailureInput {
	phase: FailurePhase;
	/** DOMException.name；不是 DOMException 的失败传 null */
	errorName: string | null;
	/** 连续第几次失败（从 1 开始）；任何一次成功都要归零 */
	attempt: number;
}

export interface RecoveryPlan {
	kind: FailureKind;
	/** null 表示不自动重试 */
	retryAfterMs: number | null;
	/** 等待态那一行说明；与诊断条的归因口径一致 */
	message: string;
}

/** 设备已经不在（拔插事件与传输层 NotFoundError 共用同一句） */
export const GONE_MESSAGE = '设备已经断开，插好后会自动接回~';

const BUSY_MESSAGE = '设备被别的程序占着，正在等它让出来~';

/**
 * 退避节奏：1s → 2s → 5s，之后固定 5s。
 *
 * 占用时长由烧录过程决定（可能几分钟），所以封顶后不放弃重试；上限只有 5s，
 * 烧录器一退出就能很快接回。设备不在时每次尝试只是一次 getDevices() 与一次重开，
 * 开销可以忽略，因此「设备不在」也照这个节奏复查，而不是只等事件——句柄假死但
 * 设备没真拔走时不会有任何事件。
 */
const BACKOFF_MS = [1000, 2000, 5000] as const;

/** 第 attempt 次失败后该等多久（attempt 从 1 开始） */
export function backoffMs(attempt: number): number {
	const index = Math.min(Math.max(attempt, 1), BACKOFF_MS.length) - 1;
	return BACKOFF_MS[index];
}

export function planRecovery(input: FailureInput): RecoveryPlan {
	const { phase, errorName, attempt } = input;
	const wait = backoffMs(attempt);

	switch (errorName) {
		case 'AccessDeniedError':
			return { kind: 'busy', retryAfterMs: wait, message: BUSY_MESSAGE };
		case 'NetworkError':
			// Windows 上被烧录器占着就落在传输这一支：Chrome 的 open() 只建句柄对象，
			// 真正的 WinUsb 打开发生在第一次传输
			return phase === 'transfer'
				? { kind: 'busy', retryAfterMs: wait, message: BUSY_MESSAGE }
				: { kind: 'driver', retryAfterMs: wait, message: '设备打不开，驱动没就绪或者被占着，正在重试~' };
		case 'NotFoundError':
			return { kind: 'gone', retryAfterMs: wait, message: GONE_MESSAGE };
		case 'SecurityError':
			return { kind: 'fatal', retryAfterMs: null, message: '浏览器拦下了这次调用，点「连接设备」重试' };
		default:
			break;
	}

	// 打开阶段的其它失败基本都是驱动/枚举层没就绪；传输阶段的失败最可能是句柄失效
	// 或被别的进程打断（AbortError、InvalidStateError 都落在这里），两种情况都靠
	// 「等一会儿重开设备」自愈，不必让界面停在永久错误上。
	return phase === 'open'
		? { kind: 'driver', retryAfterMs: wait, message: '设备还没就绪，正在重试~' }
		: { kind: 'busy', retryAfterMs: wait, message: BUSY_MESSAGE };
}
