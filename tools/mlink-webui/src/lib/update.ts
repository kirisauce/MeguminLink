/**
 * 更新模式 CDC 串口上的固件升级协议：帧编解码、停止等待通道与镜像本地预检。
 *
 * 契约的唯一权威在固件侧 main/usb/firmware_updater.cc 顶部。帧是 8B 头 + 载荷，
 * 整数一律大端：magic u16（0x4D4F）+ type u8 + status u8 + length u16 + seq u16。
 * 主机严格停等——发一帧、等它对应的响应，读到响应才发下一帧；响应错位、载荷声
 * 明长度越界、status 非 ok 都在通道层失败。
 *
 * 本模块只认一对字节流，不碰 navigator 与 SerialPort：串口由调用方打开后把
 * readable/writable 交进来，协议因此能在宿主机上喂一台假设备跑用例。
 */

export const UPDATE_VID = 0x303a;
export const UPDATE_PID = 0x4020;
/** 原生 USB CDC 不依赖线速，但打开端口必须给一个值。 */
export const UPDATE_BAUD = 115200;

/** 单个数据帧的最大载荷，与固件的 UPDATE_CHUNK_MAX 一致。 */
export const UPDATE_CHUNK_MAX = 4096;

const FRAME_MAGIC = 0x4d4f;
const FRAME_HEADER_SIZE = 8;
/** 响应载荷上限（Info = 槽位 u32 + 分块 u32 + 版本 32B + MAC 6B）：帧头声明的长度越界即按坏流处理。 */
const RESPONSE_PAYLOAD_MAX = 2 * 4 + 32 + 6;

export const CMD_UPDATE_INFO = 0x01;
export const CMD_UPDATE_BEGIN = 0x02;
export const CMD_UPDATE_DATA = 0x03;
export const CMD_UPDATE_FINISH = 0x04;

/** Info / Begin / Finish 的响应超时：Begin 要擦除镜像覆盖的范围，Finish 要读回整个分区校验。 */
export const UPDATE_LONG_TIMEOUT_MS = 10000;
/** 数据帧只做一段 flash 写入：给一个较短的上限，异常时不必陪跑。 */
export const UPDATE_DATA_TIMEOUT_MS = 3000;
/** 挑串口时的握手超时：只问一句"这个口上有没有更新模式在听"，不碰 flash，按毫秒级的应答量级给。 */
export const UPDATE_PROBE_TIMEOUT_MS = 400;

export const UPDATE_STATUS_NAMES: Readonly<Record<number, string>> = {
	0x00: 'ok',
	0x02: 'malformed',
	0x03: 'internal',
	0x04: 'unsupported',
	0x06: 'no-partition',
	0x07: 'hash-mismatch',
	0x08: 'image-invalid',
	0x09: 'size-exceeded',
	0x0a: 'sequence',
	0x0b: 'flash-error',
};

/** 状态码的人话解释：界面直接显示，不猜设备为什么拒绝。 */
export const UPDATE_STATUS_TEXT: Readonly<Record<number, string>> = {
	0x02: '设备认为这条帧的载荷与命令不符',
	0x03: '设备内部错误（flash 操作或摘要运算失败）',
	0x04: '设备不认识这条命令',
	0x06: '设备没有可写的 OTA 槽（分区表里没有第二个 app 分区）',
	0x07: '设备收到的字节流与主机申报的 SHA-256 不符',
	0x08: '设备校验镜像失败（镜像格式或尾部摘要不对）',
	0x09: '镜像超过槽位大小，或累计写入超过申报值',
	0x0a: '命令顺序错误（例如没 Begin 就发 Data）',
	0x0b: '设备写 flash 失败',
};

/** 镜像尾部附加的 SHA-256 长度（esp_ota_end 读回校验的那一份）。 */
export const IMAGE_TRAILER_LEN = 32;
/** app 镜像魔数（ESP 镜像首字节）。 */
export const APP_IMAGE_MAGIC = 0xe9;
/** 镜像内 esp_app_desc_t 的偏移。 */
export const APP_DESC_OFFSET = 0x20;
const APP_DESC_MAGIC = 0xabcd5432;
/** esp_app_desc_t 里 version / project_name 相对描述符的偏移与字段宽度。 */
const DESC_VERSION_OFFSET = 16;
const DESC_PROJECT_OFFSET = 48;
const DESC_FIELD_LEN = 32;
/** 本项目的 app 工程名（CMake project），用来认出刷错工程名的镜像。 */
export const APP_PROJECT_NAME = 'megumin-link-esp';

export class UpdateError extends Error {
	/** 设备返回的状态码；传输层失败（超时、断开、错位）时为 null。 */
	readonly status: number | null;

	constructor(message: string, status: number | null = null) {
		super(message);
		this.name = 'UpdateError';
		this.status = status;
	}
}

export type UpdateFrame = {
	command: number;
	status: number;
	seq: number;
	payload: DataView;
};

/** 组一条请求帧；请求侧 status 恒为 0。 */
export function encodeFrame(command: number, seq: number, payload: Uint8Array = new Uint8Array(0)): Uint8Array {
	const frame = new Uint8Array(FRAME_HEADER_SIZE + payload.byteLength);
	const header = new DataView(frame.buffer);
	header.setUint16(0, FRAME_MAGIC, false);
	header.setUint8(2, command);
	header.setUint8(3, 0);
	header.setUint16(4, payload.byteLength, false);
	header.setUint16(6, seq, false);
	frame.set(payload, FRAME_HEADER_SIZE);
	return frame;
}

/** 解析一条完整帧（帧头 + 载荷都在这个视图里）；长度与帧头声明不符即失败。 */
export function decodeFrame(bytes: DataView): UpdateFrame {
	if (bytes.byteLength < FRAME_HEADER_SIZE) {
		throw new UpdateError(`响应帧过短（${bytes.byteLength} 字节）`);
	}
	const magic = bytes.getUint16(0, false);
	if (magic !== FRAME_MAGIC) {
		throw new UpdateError(`响应帧魔数错误：0x${magic.toString(16).padStart(4, '0')}`);
	}
	const length = bytes.getUint16(4, false);
	if (bytes.byteLength !== FRAME_HEADER_SIZE + length) {
		throw new UpdateError(`响应帧长度与帧头不符（帧头声明 ${length}，实到 ${bytes.byteLength - FRAME_HEADER_SIZE}）`);
	}
	return {
		command: bytes.getUint8(2),
		status: bytes.getUint8(3),
		seq: bytes.getUint16(6, false),
		payload: new DataView(bytes.buffer, bytes.byteOffset + FRAME_HEADER_SIZE, length),
	};
}

export type UpdateInfo = {
	/** 可写的 OTA 槽大小；0 = 设备没有第二个 app 分区 */
	slotSize: number;
	/** 设备允许的单帧最大载荷 */
	maxChunk: number;
	version: string;
	/** 设备自报 MAC（小写冒号分隔）；老固件不上报或读取失败时为 null */
	mac: string | null;
};

function readCString(bytes: DataView, offset: number, length: number): string {
	const raw = new Uint8Array(bytes.buffer, bytes.byteOffset + offset, length);
	const end = raw.indexOf(0);
	return new TextDecoder().decode(end < 0 ? raw : raw.subarray(0, end));
}

function readMac(bytes: DataView, offset: number): string | null {
	const raw = new Uint8Array(bytes.buffer, bytes.byteOffset + offset, 6);
	// 全 0 是"设备没上报身份"，不是一个 MAC
	if (raw.every((byte) => byte === 0)) return null;
	return Array.from(raw, (byte) => byte.toString(16).padStart(2, '0')).join(':');
}

/** Info 响应载荷：槽位 u32 | 分块上限 u32 | 版本 32B | MAC 6B（MAC 是后加的尾部字段）。 */
export function decodeInfo(payload: DataView): UpdateInfo {
	const versionOffset = 8;
	const macOffset = versionOffset + DESC_FIELD_LEN;
	if (payload.byteLength < versionOffset + DESC_FIELD_LEN) {
		throw new UpdateError(`Info 载荷过短（${payload.byteLength} 字节）`);
	}
	return {
		slotSize: payload.getUint32(0, false),
		maxChunk: payload.getUint32(4, false),
		version: readCString(payload, versionOffset, DESC_FIELD_LEN),
		mac: payload.byteLength >= macOffset + 6 ? readMac(payload, macOffset) : null,
	};
}

export type FirmwareImage = {
	bytes: Uint8Array<ArrayBuffer>;
	size: number;
	/** 镜像头里的版本串（esp_app_desc_t::version） */
	version: string;
	/** 镜像头里的工程名，用来认出刷错工程的镜像 */
	projectName: string;
	/** 整镜像 SHA-256：Begin 时申报给设备的那一份 */
	digest: Uint8Array;
};

async function sha256(bytes: Uint8Array<ArrayBuffer>): Promise<Uint8Array<ArrayBuffer>> {
	return new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
	return a.byteLength === b.byteLength && a.every((byte, index) => byte === b[index]);
}

/**
 * 镜像本地预检：魔数、app 描述与尾部摘要任何一项不对都在写设备之前失败——设备那边
 * 走到 Finish 才发现摘要不对时，槽位已经被擦过一遍了。
 *
 * 尾部摘要指镜像最后 32 字节，它必须等于其前全部内容的 SHA-256（esp_ota_end 读回
 * 校验的就是这一份，idf.py 打包时写进镜像）。
 */
export async function readFirmwareImage(bytes: Uint8Array<ArrayBuffer>): Promise<FirmwareImage> {
	if (bytes.byteLength <= APP_DESC_OFFSET + DESC_FIELD_LEN + IMAGE_TRAILER_LEN) {
		throw new UpdateError(`镜像只有 ${bytes.byteLength} 字节，不是一个 app 镜像`);
	}
	if (bytes[0] !== APP_IMAGE_MAGIC) {
		throw new UpdateError(`镜像魔数错误：0x${bytes[0].toString(16).padStart(2, '0')}（app 镜像应为 0xe9）`);
	}
	const descMagic = new DataView(bytes.buffer, bytes.byteOffset + APP_DESC_OFFSET, 4).getUint32(0, true);
	if (descMagic !== APP_DESC_MAGIC) {
		throw new UpdateError('镜像里没有 app 描述结构，可能是合并镜像或分区表');
	}

	const trailer = bytes.subarray(bytes.byteLength - IMAGE_TRAILER_LEN);
	const body = bytes.subarray(0, bytes.byteLength - IMAGE_TRAILER_LEN);
	if (!sameBytes(await sha256(body), trailer)) {
		throw new UpdateError('镜像尾部 SHA-256 与内容不符（文件损坏或被截断）');
	}

	const desc = new DataView(bytes.buffer, bytes.byteOffset + APP_DESC_OFFSET);
	return {
		bytes,
		size: bytes.byteLength,
		version: readCString(desc, DESC_VERSION_OFFSET, DESC_FIELD_LEN),
		projectName: readCString(desc, DESC_PROJECT_OFFSET, DESC_FIELD_LEN),
		digest: await sha256(bytes),
	};
}

/**
 * 等待中的字节队列：读泵往里塞，取字节的人按需等够长度。
 *
 * 串口是字节流，响应帧边界只由帧头声明的长度决定，因此取字节必须能跨越多次 USB
 * 传输；一次 read 拿到的分片数完全由主机侧调度决定，不能假设一条帧一次到齐。
 */
class ByteQueue {
	#chunks: Uint8Array[] = [];
	#buffered = 0;
	#failure: unknown = null;
	#wake: (() => void) | null = null;

	push(chunk: Uint8Array): void {
		this.#chunks.push(chunk);
		this.#buffered += chunk.byteLength;
		this.#signal();
	}

	fail(error: unknown): void {
		this.#failure ??= error;
		this.#signal();
	}

	#signal(): void {
		const wake = this.#wake;
		this.#wake = null;
		wake?.();
	}

	#wait(ms: number): Promise<void> {
		return new Promise<void>((resolve) => {
			let settled = false;
			const finish = (): void => {
				if (settled) return;
				settled = true;
				clearTimeout(timer);
				this.#wake = null;
				resolve();
			};
			const timer = setTimeout(finish, ms);
			this.#wake = finish;
		});
	}

	/** 等够 n 字节再原子取出；超时或流已断都在这里失败。 */
	async take(count: number, timeoutMs: number, what: string): Promise<Uint8Array> {
		const deadline = Date.now() + timeoutMs;
		while (this.#buffered < count) {
			if (this.#failure !== null) {
				throw new UpdateError(`${what}失败：${describe(this.#failure)}`);
			}
			const remaining = deadline - Date.now();
			if (remaining <= 0) {
				throw new UpdateError(`${what}超时（${timeoutMs} ms 内只到了 ${this.#buffered}/${count} 字节）`);
			}
			await this.#wait(remaining);
		}

		const out = new Uint8Array(count);
		let filled = 0;
		while (filled < count) {
			const chunk = this.#chunks[0];
			const need = count - filled;
			if (chunk.byteLength <= need) {
				out.set(chunk, filled);
				filled += chunk.byteLength;
				this.#chunks.shift();
			} else {
				out.set(chunk.subarray(0, need), filled);
				this.#chunks[0] = chunk.subarray(need);
				filled += need;
			}
		}
		this.#buffered -= count;
		return out;
	}
}

function describe(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

/** 串口打开后交出的一对字节流；Web Serial 的 port.readable / port.writable 结构上满足它。 */
export type UpdateStreams = {
	readable: ReadableStream<Uint8Array>;
	writable: WritableStream<Uint8Array>;
};

/**
 * 协议通道：把一对字节流包成"发一条命令、拿到它的响应"。
 *
 * seq 由本对象生成并核对回显：响应错位说明主机与设备的帧边界已经对不上，此时继续
 * 写下去只会把镜像写歪，所以一律失败。
 */
export class UpdateChannel {
	#reader: ReadableStreamDefaultReader<Uint8Array>;
	#writer: WritableStreamDefaultWriter<Uint8Array>;
	#queue = new ByteQueue();
	#pump: Promise<void>;
	#seq = 0;
	#closed = false;

	constructor(streams: UpdateStreams) {
		this.#reader = streams.readable.getReader();
		this.#writer = streams.writable.getWriter();
		this.#pump = this.#readLoop();
	}

	async #readLoop(): Promise<void> {
		try {
			for (;;) {
				const { value, done } = await this.#reader.read();
				if (done) break;
				if (value !== undefined && value.byteLength > 0) this.#queue.push(value);
			}
			this.#queue.fail(new Error('串口已关闭'));
		} catch (error) {
			this.#queue.fail(error);
		}
	}

	async #write(bytes: Uint8Array): Promise<void> {
		try {
			await this.#writer.write(bytes);
		} catch (error) {
			throw new UpdateError(`串口写入失败：${describe(error)}`);
		}
	}

	/** 发一条命令并等它对应的响应；超时、错位、status 非 ok 都抛 UpdateError。 */
	async request(command: number, payload: Uint8Array, timeoutMs: number): Promise<DataView> {
		const seq = (this.#seq = (this.#seq + 1) & 0xffff);
		const label = `命令 0x${command.toString(16).padStart(2, '0')}/seq ${seq} 的响应`;

		await this.#write(encodeFrame(command, seq, payload));

		const header = await this.#queue.take(FRAME_HEADER_SIZE, timeoutMs, label);
		const declared = new DataView(header.buffer, header.byteOffset, header.byteLength).getUint16(4, false);
		if (declared > RESPONSE_PAYLOAD_MAX) {
			throw new UpdateError(`${label}声明了 ${declared} 字节载荷，超出协议上限`);
		}
		const body = declared === 0 ? new Uint8Array(0) : await this.#queue.take(declared, timeoutMs, label);

		const frame = new Uint8Array(FRAME_HEADER_SIZE + declared);
		frame.set(header, 0);
		frame.set(body, FRAME_HEADER_SIZE);

		const parsed = decodeFrame(new DataView(frame.buffer));
		if (parsed.command !== command || parsed.seq !== seq) {
			throw new UpdateError(
				`响应错位：命令 0x${command.toString(16).padStart(2, '0')}/seq ${seq} 收到 ` +
					`0x${parsed.command.toString(16).padStart(2, '0')}/seq ${parsed.seq}`
			);
		}
		if (parsed.status !== 0x00) {
			const name = UPDATE_STATUS_NAMES[parsed.status] ?? `0x${parsed.status.toString(16)}`;
			const text = UPDATE_STATUS_TEXT[parsed.status];
			// 解释随状态码一起进消息：界面只需要显示 error.message
			throw new UpdateError(text === undefined ? `设备返回 status=${name}` : `设备返回 status=${name}：${text}`, parsed.status);
		}
		return parsed.payload;
	}

	/** 结束会话并释放流上的锁；端口本身由调用方关闭。 */
	async close(): Promise<void> {
		if (this.#closed) return;
		this.#closed = true;
		try {
			await this.#reader.cancel();
		} catch {
			// 设备已经复位、端口已经没了：此时没有什么可取消的
		}
		await this.#pump;
		this.#reader.releaseLock();
		try {
			await this.#writer.close();
		} catch {
			// 端口已经断开时底层流会拒绝关闭，锁仍要释放
		}
		this.#writer.releaseLock();
	}
}

/** 进度阶段：擦除与校验的等待都发生在设备端，界面据此给出不同的提示。 */
export type FlashPhase = 'info' | 'erase' | 'write' | 'verify' | 'done';

export type FlashProgress = {
	phase: FlashPhase;
	/** 已被设备确认写入的字节数（write 阶段之外为 0 或总数） */
	written: number;
	total: number;
};

export type FlashHooks = {
	onProgress?: (progress: FlashProgress) => void;
	/** Info 拿到后立刻回调一次：界面据此显示设备身份与它当前跑的固件版本。 */
	onInfo?: (info: UpdateInfo) => void;
};

export type FlashResult = {
	bytes: number;
	chunk: number;
	/** 设备在写入前跑的固件版本 */
	deviceVersion: string;
	deviceMac: string | null;
};

/**
 * Begin 载荷：镜像大小 u32 | 整镜像 SHA-256。
 *
 * 固件按 `sizeof(u32) + UPDATE_DIGEST_LEN` 严格校验长度，多一个字节都回 malformed。
 */
export function encodeBegin(size: number, digest: Uint8Array): Uint8Array {
	const payload = new Uint8Array(4 + digest.byteLength);
	new DataView(payload.buffer).setUint32(0, size, false);
	payload.set(digest, 4);
	return payload;
}

/**
 * 已经握过手的更新会话：身份拿在手里，通道还开着。
 *
 * 先开会话再灌镜像，是因为串口那头未必是目标设备——Begin 会把槽擦掉，认错了就白擦。
 */
export class UpdateSession {
	readonly info: UpdateInfo;
	readonly #channel: UpdateChannel;

	private constructor(channel: UpdateChannel, info: UpdateInfo) {
		this.#channel = channel;
		this.info = info;
	}

	/**
	 * 开一条会话：第一条 Info 就是握手。
	 *
	 * 失败时通道在这里收干净；成功时留给 flash 复用，同一个口不重开。
	 */
	static async open(streams: UpdateStreams, timeoutMs = UPDATE_PROBE_TIMEOUT_MS): Promise<UpdateSession> {
		const channel = new UpdateChannel(streams);
		try {
			const info = decodeInfo(await channel.request(CMD_UPDATE_INFO, new Uint8Array(0), timeoutMs));
			return new UpdateSession(channel, info);
		} catch (cause) {
			await channel.close();
			throw cause;
		}
	}

	/**
	 * 把镜像推进空闲 OTA 槽：Begin（擦除）→ Data 分块 → Finish（校验并切换启动分区）。
	 *
	 * 设备两级校验（字节流对申报摘要、镜像对自身尾部摘要）不过就不切换启动分区，旧固件
	 * 照常运行；Finish 之后设备自行软复位，本函数不负责等它回来。
	 */
	async flash(image: FirmwareImage, hooks: FlashHooks = {}): Promise<FlashResult> {
		const channel = this.#channel;
		const total = image.size;
		const info = this.info;
		hooks.onInfo?.(info);
		if (info.slotSize === 0) throw new UpdateError(UPDATE_STATUS_TEXT[0x06]);
		if (total > info.slotSize) {
			throw new UpdateError(`镜像 ${total} 字节超过设备的 OTA 槽 ${info.slotSize} 字节`);
		}

		const chunk = Math.min(info.maxChunk, UPDATE_CHUNK_MAX, total);
		if (chunk <= 0) throw new UpdateError(`设备上报的分块上限非法：${info.maxChunk}`);

		hooks.onProgress?.({ phase: 'erase', written: 0, total });
		await channel.request(CMD_UPDATE_BEGIN, encodeBegin(total, image.digest), UPDATE_LONG_TIMEOUT_MS);

		const block = new Uint8Array(chunk);
		let written = 0;
		while (written < total) {
			const take = Math.min(chunk, total - written);
			block.set(image.bytes.subarray(written, written + take));
			const payload = await channel.request(CMD_UPDATE_DATA, block.subarray(0, take), UPDATE_DATA_TIMEOUT_MS);
			if (payload.byteLength !== 4) {
				throw new UpdateError(`Data 响应应为 4 字节累计值，实到 ${payload.byteLength} 字节`);
			}
			// 以设备确认的累计值为准：主机与设备对"写到哪"必须始终一致
			const acked = payload.getUint32(0, false);
			if (acked !== written + take) {
				throw new UpdateError(`设备累计写入 ${acked} 字节，与主机发出的 ${written + take} 字节不符`);
			}
			written = acked;
			hooks.onProgress?.({ phase: 'write', written, total });
		}

		hooks.onProgress?.({ phase: 'verify', written, total });
		await channel.request(CMD_UPDATE_FINISH, new Uint8Array(0), UPDATE_LONG_TIMEOUT_MS);
		hooks.onProgress?.({ phase: 'done', written, total });

		return { bytes: total, chunk, deviceVersion: info.version, deviceMac: info.mac };
	}

	/** 结束会话并释放流上的锁；端口本身由调用方关闭。 */
	async close(): Promise<void> {
		await this.#channel.close();
	}
}

/** 一次性的刷入：开会话（Info 按正式命令的量级等）→ 灌镜像 → 关会话。 */
export async function flashFirmware(
	streams: UpdateStreams,
	image: FirmwareImage,
	hooks: FlashHooks = {}
): Promise<FlashResult> {
	hooks.onProgress?.({ phase: 'info', written: 0, total: image.size });
	const session = await UpdateSession.open(streams, UPDATE_LONG_TIMEOUT_MS);
	try {
		return await session.flash(image, hooks);
	} finally {
		await session.close();
	}
}
