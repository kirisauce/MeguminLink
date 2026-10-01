<script lang="ts">
	import { formatBytes, formatRate } from '../../../lib/rate';
	import type { MlinkSession } from '../../../lib/session.svelte';
	import {
		APP_PROJECT_NAME,
		UpdateError,
		flashFirmware,
		readFirmwareImage,
		type FirmwareImage,
		type FlashPhase,
		type FlashProgress,
	} from '../../../lib/update';
	import { acquireUpdatePort, canFlashFromBrowser, waitForUpdateMode, type UpdatePort } from '../../../lib/update_port';

	let { session, open, onclose }: { session: MlinkSession; open: boolean; onclose: () => void } = $props();

	/**
	 * 流程阶段：idle 待机、checking 读文件、entering 发进入命令、waiting 等重新枚举、
	 * port 取串口、flashing 烧录、done/failed 收尾。烧录期间不许关，避免把在途会话拆了。
	 */
	type Stage = 'idle' | 'checking' | 'entering' | 'waiting' | 'port' | 'flashing' | 'done' | 'failed';

	const PHASE_TEXT: Readonly<Record<FlashPhase, string>> = {
		info: '读取设备信息',
		erase: '设备正在擦除目标槽',
		write: '写入中',
		verify: '设备正在校验镜像',
		done: '写入完成',
	};

	let stage = $state<Stage>('idle');
	let detail = $state('');
	let error = $state<string | null>(null);
	/** 设备没上报 MAC（老固件）时的说明：只提示，不拦。 */
	let identityNote = $state<string | null>(null);
	let firmware = $state<FirmwareImage | null>(null);
	let fileName = $state('');
	let dragging = $state(false);
	let written = $state(0);
	let percent = $state<number | null>(null);
	let rate = $state<number | null>(null);
	let elapsed = $state<number | null>(null);

	let panel = $state<HTMLElement | null>(null);
	/** 写入阶段的起点：擦除耗时不该算进速率。 */
	let writeStartedAt: number | null = null;

	const busy = $derived(
		stage === 'checking' || stage === 'entering' || stage === 'waiting' || stage === 'port' || stage === 'flashing'
	);
	/**
	 * 这次烧录认准的设备串号：进更新模式那一刻 WebUSB 侧就失去目标了，之后要拿串口上
	 * 自报的 MAC 跟它比对，所以必须在动手之前记下来。
	 */
	let startedWith = $state<string | null>(null);
	const target = $derived(startedWith ?? session.deviceSerial);
	const foreign = $derived(firmware !== null && firmware.projectName !== APP_PROJECT_NAME);
	/**
	 * 没有 Web Serial 就不必让设备白重启一趟：先说清楚，并把开始按钮置灰。
	 * 初值按可用算——这段标记也会在构建期于 Node 上渲染，那时没有 navigator。
	 */
	let flashable = $state(true);
	$effect(() => {
		flashable = canFlashFromBrowser();
	});

	/** 清掉上一次的进度；选好的文件留着（连续刷两次同一份是常见动作）。 */
	function resetProgress(): void {
		stage = 'idle';
		detail = '';
		error = null;
		identityNote = null;
		written = 0;
		percent = null;
		rate = null;
		elapsed = null;
		writeStartedAt = null;
		startedWith = null;
	}

	// 每次打开都从待机开始：上一次的绿色成功条不该跟着新一次会话
	$effect(() => {
		if (open) resetProgress();
		panel?.focus();
	});

	async function load(file: File): Promise<void> {
		stage = 'checking';
		detail = '读取镜像';
		error = null;
		identityNote = null;
		try {
			firmware = await readFirmwareImage(new Uint8Array(await file.arrayBuffer()));
			fileName = file.name;
			stage = 'idle';
			detail = '';
		} catch (cause) {
			firmware = null;
			fileName = file.name;
			stage = 'failed';
			error = cause instanceof Error ? cause.message : String(cause);
		}
	}

	function onDrop(event: DragEvent): void {
		event.preventDefault();
		dragging = false;
		const file = event.dataTransfer?.files?.[0];
		if (file !== undefined) void load(file);
	}

	function onPick(event: Event): void {
		const input = event.currentTarget as HTMLInputElement;
		const file = input.files?.[0];
		if (file !== undefined) void load(file);
	}

	function onProgress(progress: FlashProgress): void {
		detail = PHASE_TEXT[progress.phase];
		written = progress.written;
		if (progress.phase === 'write') {
			writeStartedAt ??= Date.now();
			const seconds = (Date.now() - writeStartedAt) / 1000;
			rate = seconds > 0 ? progress.written / seconds : null;
			percent = Math.floor((progress.written / progress.total) * 100);
			return;
		}
		percent = progress.phase === 'done' ? 100 : null;
	}

	/**
	 * 进更新模式 → 取串口 → 刷入。
	 *
	 * 身份核对放在 Info 之后、Begin 之前：设备在这个回调里被拒绝时还一个字节都没写，
	 * 只是白重启了一次。
	 */
	async function start(): Promise<void> {
		const image = firmware;
		if (image === null || busy) return;

		resetProgress();
		stage = 'entering';
		let port: UpdatePort | null = null;

		try {
			if (session.device === null) throw new Error('设备已经断开，先重新连接再烧录');
			startedWith = session.deviceSerial;

			detail = '让设备重启进更新模式';
			await session.enterUpdateMode();
			if (session.commandError !== null) {
				throw new Error(`设备没有接受进入更新模式：${session.commandError}`);
			}

			stage = 'waiting';
			detail = '等设备以更新模式重新枚举';
			if (!(await waitForUpdateMode(startedWith))) {
				throw new Error('10 秒内没等到设备进更新模式，看看设备上的灯是不是在跑渐变');
			}

			stage = 'port';
			detail = '取更新模式的串口';
			port = await acquireUpdatePort();
			if (port === null) {
				// 用户在选择器里点了取消：回到待机，什么都没发生
				stage = 'idle';
				detail = '';
				return;
			}

			stage = 'flashing';
			detail = '读取设备信息';
			const expected = startedWith;
			const startedAt = Date.now();
			const result = await flashFirmware(port.streams, image, {
				onInfo: (info) => {
					if (info.mac === null) {
						identityNote = '设备没有上报 MAC（老固件），这次没法确认是不是同一台';
						return;
					}
					if (expected !== null && info.mac.toLowerCase() !== expected.toLowerCase()) {
						throw new UpdateError(`串口上那台是 ${info.mac}，不是当前设备 ${expected}：一个字节都没写`);
					}
				},
				onProgress,
			});

			stage = 'done';
			detail = '设备已切换启动分区，正在重启进新固件';
			elapsed = (Date.now() - startedAt) / 1000;
			written = result.bytes;
			percent = 100;
		} catch (cause) {
			stage = 'failed';
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			await port?.close();
		}
	}

	function dismiss(): void {
		if (!busy) onclose();
	}

	function onkeydown(event: KeyboardEvent): void {
		if (!open) return;
		if (event.key === 'Escape') {
			// 捕获阶段拦下：同一个 Esc 不能再去冒泡给下层监听器
			event.preventDefault();
			event.stopPropagation();
			dismiss();
			return;
		}
		if (event.key !== 'Tab' || panel === null) return;
		// 弹窗是 modal：Tab 只在面板里的可用元素之间循环
		const focusable = [
			...panel.querySelectorAll<HTMLElement>(
				'button:not(:disabled), input:not(:disabled), [href], [tabindex]:not([tabindex="-1"])'
			),
		];
		if (focusable.length === 0) return;
		event.preventDefault();
		event.stopPropagation();
		const index = focusable.indexOf(document.activeElement as HTMLElement);
		const step = event.shiftKey ? -1 : 1;
		const next =
			index < 0
				? focusable[event.shiftKey ? focusable.length - 1 : 0]
				: focusable[(index + step + focusable.length) % focusable.length];
		next.focus();
	}
</script>

<!-- 与抽屉同一套约定：捕获阶段拦 Esc/Tab，不让按键漏到下层浮层 -->
<svelte:window onkeydowncapture={onkeydown} />

<!-- 常驻 DOM：class 切换驱动进出 transition；关闭态由 visibility + inert 兜住交互 -->
<button
	class="scrim"
	class:open
	type="button"
	tabindex="-1"
	aria-label="关闭烧录窗口"
	onclick={dismiss}
></button>

<div
	class="dialog"
	class:open
	inert={!open}
	role="dialog"
	aria-modal="true"
	aria-labelledby="update-title"
	tabindex="-1"
	bind:this={panel}
>
	<h2 id="update-title">烧录固件</h2>

	<p class="hint">
		只能刷 app 镜像（<code>megumin-link-esp.bin</code>）。设备会先软复位进更新模式，烧完自己重启回正常模式。
	</p>

	<label
		class="drop"
		class:over={dragging}
		ondragover={(event) => {
			event.preventDefault();
			dragging = true;
		}}
		ondragleave={() => (dragging = false)}
		ondrop={onDrop}
	>
		<input
			class="sr-only"
			type="file"
			accept=".bin,application/octet-stream"
			disabled={busy}
			onchange={onPick}
		/>
		<span class="drop-title">{fileName === '' ? '选择固件文件' : fileName}</span>
		<span class="drop-hint">点这里选，或把 .bin 拖进来</span>
	</label>

	{#if firmware !== null}
		<dl class="meta">
			<div><dt>大小</dt><dd>{formatBytes(firmware.size)}</dd></div>
			<div><dt>版本</dt><dd>{firmware.version === '' ? '—' : firmware.version}</dd></div>
			<div><dt>工程</dt><dd>{firmware.projectName === '' ? '—' : firmware.projectName}</dd></div>
			<div><dt>目标设备</dt><dd>{target ?? '—'}</dd></div>
		</dl>
		{#if foreign}
			<p class="warn">这个镜像的工程名不是 {APP_PROJECT_NAME}，刷进去设备可能起不来。</p>
		{/if}
	{/if}

	{#if busy || stage === 'done' || stage === 'failed'}
		<div class="progress">
			<div
				class="track"
				role="progressbar"
				aria-label="烧录进度"
				aria-valuemin="0"
				aria-valuemax="100"
				aria-valuenow={percent ?? undefined}
			>
				<div
					class="fill"
					class:indeterminate={percent === null}
					class:ok={stage === 'done'}
					class:bad={stage === 'failed'}
					style:width={percent === null ? undefined : `${percent}%`}
				></div>
			</div>
			<p class="status">
				<span>{detail}</span>
				<span class="numbers">
					{#if percent !== null}{percent}%{/if}
					{#if stage === 'flashing' || stage === 'done'}
						· {formatBytes(written)}
						{#if rate !== null}· {formatRate(rate)}{/if}
					{/if}
					{#if elapsed !== null}· 用时 {elapsed.toFixed(1)}s{/if}
				</span>
			</p>
		</div>
	{/if}

	{#if !flashable}
		<p class="warn">这个浏览器没有 Web Serial，网页烧录需要 Chrome 或 Edge；也可以用工具页的 mlinkctl。</p>
	{/if}

	{#if identityNote !== null}
		<p class="warn">{identityNote}</p>
	{/if}
	{#if error !== null}
		<p class="error">{error}</p>
	{/if}
	{#if stage === 'done'}
		<p class="ok">设备重启后页面会自动接回这台，等它回来就能看到新版本号了。</p>
	{/if}

	<div class="buttons">
		<button class="button" type="button" disabled={busy} onclick={dismiss}>
			{stage === 'done' ? '关闭' : '取消'}
		</button>
		<button
			class="button filled"
			type="button"
			disabled={busy || firmware === null || !flashable}
			onclick={() => void start()}
		>
			{stage === 'failed' ? '重试' : '开始烧录'}
		</button>
	</div>
</div>

<style>
	.scrim {
		position: fixed;
		inset: 0;
		z-index: 20;
		padding: 0;
		border: none;
		background: var(--md-sys-color-scrim);
		opacity: 0;
		visibility: hidden;
		pointer-events: none;
		transition:
			opacity var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit),
			visibility 0s linear var(--md-sys-motion-duration-exit);
	}

	.scrim.open {
		opacity: 1;
		visibility: visible;
		pointer-events: auto;
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			visibility 0s;
	}

	.dialog {
		position: fixed;
		top: 50%;
		left: 50%;
		z-index: 21;
		width: min(32rem, calc(100vw - 2 * var(--md-sys-space-xl)));
		padding: var(--md-sys-space-xl);
		border-radius: var(--md-sys-shape-corner-xl);
		background: var(--md-sys-color-surface-container-high);
		box-shadow: var(--md-sys-elevation-1);
		opacity: 0;
		visibility: hidden;
		transform: translate(-50%, -50%) perspective(1200px) rotateX(6deg) rotateY(-4.5deg) scale(0.9);
		transition:
			opacity var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit),
			transform var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit),
			visibility 0s linear var(--md-sys-motion-duration-exit);
	}

	.dialog.open {
		opacity: 1;
		visibility: visible;
		transform: translate(-50%, -50%) perspective(1200px) rotateX(0deg) rotateY(0deg) scale(1);
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			transform var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			visibility 0s;
	}

	h2 {
		margin: 0 0 var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-headline-sm);
		font-weight: 600;
	}

	.hint,
	.warn,
	.error,
	.ok,
	.status {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	.hint code {
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-sm);
	}

	.drop {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xs);
		margin-top: var(--md-sys-space-lg);
		padding: var(--md-sys-space-lg);
		border: 1px dashed var(--md-sys-color-outline);
		border-radius: var(--md-sys-shape-corner-md);
		cursor: pointer;
		text-align: center;
		transition:
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.drop:hover,
	.drop.over {
		border-color: var(--md-sys-color-primary);
		background: var(--md-sys-color-surface-container);
	}

	.drop-title {
		font-size: var(--md-sys-typescale-title-md);
		color: var(--md-sys-color-on-surface);
		overflow-wrap: anywhere;
	}

	.drop-hint {
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	.sr-only {
		position: absolute;
		width: 1px;
		height: 1px;
		overflow: hidden;
		clip-path: inset(50%);
	}

	.meta {
		display: grid;
		grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr));
		gap: var(--md-sys-space-sm) var(--md-sys-space-lg);
		margin: var(--md-sys-space-lg) 0 0;
	}

	.meta div {
		display: flex;
		flex-direction: column;
		gap: 2px;
	}

	.meta dt {
		font-size: var(--md-sys-typescale-label-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	.meta dd {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		font-family: var(--md-sys-typescale-font-mono);
		overflow-wrap: anywhere;
	}

	.progress {
		margin-top: var(--md-sys-space-lg);
	}

	.track {
		position: relative;
		height: 6px;
		overflow: hidden;
		border-radius: var(--md-sys-shape-corner-full);
		background: var(--md-sys-color-surface-container);
	}

	.fill {
		height: 100%;
		border-radius: inherit;
		background: var(--md-sys-color-primary);
		transition: width var(--md-sys-motion-duration-short) linear;
	}

	/* 没有百分比可算的阶段（擦除、校验）用一段来回扫的条，而不是假装一个数字 */
	.fill.indeterminate {
		width: 28%;
		animation: sweep 1.15s var(--md-sys-motion-easing-enter) infinite;
	}

	@keyframes sweep {
		from {
			transform: translateX(-100%);
		}
		to {
			transform: translateX(400%);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.fill.indeterminate {
			animation: none;
			width: 100%;
		}
	}

	.fill.ok {
		background: var(--mlink-status-ok);
	}

	.fill.bad {
		background: var(--mlink-status-error);
	}

	.status {
		display: flex;
		justify-content: space-between;
		gap: var(--md-sys-space-md);
		margin-top: var(--md-sys-space-sm);
	}

	.numbers {
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-sm);
		white-space: nowrap;
	}

	.warn {
		margin-top: var(--md-sys-space-md);
		color: var(--mlink-status-busy);
	}

	.error {
		margin-top: var(--md-sys-space-md);
		color: var(--md-sys-color-error);
	}

	.ok {
		margin-top: var(--md-sys-space-md);
		color: var(--mlink-status-ok);
	}

	.buttons {
		display: flex;
		justify-content: flex-end;
		gap: var(--md-sys-space-sm);
		margin-top: var(--md-sys-space-xl);
	}

	.button {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.5rem 1.1rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.button:hover:not(:disabled) {
		background: var(--md-sys-color-surface-container);
	}

	.button:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.filled {
		background: var(--md-sys-color-primary);
		color: var(--md-sys-color-on-primary);
	}

	.filled:hover:not(:disabled) {
		background: color-mix(in srgb, var(--md-sys-color-primary) 92%, var(--md-sys-color-on-primary));
	}
</style>
