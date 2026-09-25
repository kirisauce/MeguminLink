<script lang="ts">
	import {
		ARQ_STATE_BROKEN,
		ARQ_STATE_IN_FLIGHT_MASK,
		ARQ_STATE_IN_FLIGHT_SHIFT,
		ARQ_STATE_WINDOW_MASK,
		arqRetxRate
	} from '../../../../lib/protocol';
	import type { LinkConfigPatch } from '../../../../lib/protocol';
	import type { MlinkSession } from '../../../../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	const busy = $derived(session.commandPending || session.device === null);
	/** 固件上报不出这组参数：既不认识 0x1B 命令，也读不到配置项。 */
	const unsupported = $derived(session.config !== null && session.config.arqRtoMinMs === null);

	/** 表单绑定值（设备值每次刷新都重置）。参数是数值，undefined = 无值。 */
	let rtoMin = $state<number | undefined>(undefined);
	let rtoMax = $state<number | undefined>(undefined);
	let maxRetx = $state<number | undefined>(undefined);
	let breaker = $state<number | undefined>(undefined);

	function syncFromDevice(): void {
		const config = session.config;
		rtoMin = config?.arqRtoMinMs ?? undefined;
		rtoMax = config?.arqRtoMaxMs ?? undefined;
		maxRetx = config?.arqMaxRetx ?? undefined;
		breaker = config?.arqBreaker ?? undefined;
	}

	$effect(syncFromDevice);

	/**
	 * 一次提交整组参数：设备每应用一次都会重置可靠传输会话（两端重新对齐序号并
	 * 同步参数），逐项提交会把会话反复打断。越界或非数字的输入一律忽略并回落到
	 * 设备当前值，不猜意图。
	 */
	function commit(): void {
		const values = [rtoMin, rtoMax, maxRetx, breaker];
		if (values.some((value) => !Number.isInteger(value))) {
			syncFromDevice();
			return;
		}
		if ((rtoMin as number) < 1 || (rtoMax as number) < (rtoMin as number)) {
			syncFromDevice();
			return;
		}
		if ((maxRetx as number) < 1 || (breaker as number) < 0) {
			syncFromDevice();
			return;
		}

		const patch: LinkConfigPatch = {
			rtoMinMs: rtoMin,
			rtoMaxMs: rtoMax,
			maxRetx,
			breaker
		};
		void session.setLinkConfig(patch);
	}

	const dirty = $derived(
		session.config !== null &&
			(rtoMin !== session.config.arqRtoMinMs ||
				rtoMax !== session.config.arqRtoMaxMs ||
				maxRetx !== session.config.arqMaxRetx ||
				breaker !== session.config.arqBreaker)
	);

	/** 运行读数：状态字节拆出窗口/在途/熔断，重传率由 tx 与 retx 现算。 */
	const readout = $derived.by(() => {
		const stats = session.stats;
		if (stats?.arqState == null) return null;

		const window_ = stats.arqState & ARQ_STATE_WINDOW_MASK;
		const inFlight = (stats.arqState >> ARQ_STATE_IN_FLIGHT_SHIFT) & ARQ_STATE_IN_FLIGHT_MASK;
		const broken = (stats.arqState & ARQ_STATE_BROKEN) !== 0;
		const rate = arqRetxRate(stats);

		return [
			`窗口 ${window_}`,
			`在途 ${inFlight}`,
			broken ? '已熔断' : '正常',
			`重传 ${stats.arqRetx ?? 0}/${stats.arqTx ?? 0}（${rate === null ? '—' : `${rate.toFixed(1)}%`}）`,
			`丢弃 ${stats.arqFailed ?? 0}`,
			`去重 ${stats.arqDup ?? 0}`
		].join(' · ');
	});
</script>

<div class="link-settings">
	<div class="grid">
		<div class="field">
			<label class="label" for="arq-rto-min">RTO 下限 (ms)</label>
			<input id="arq-rto-min" type="number" min="1" max="65535" bind:value={rtoMin} disabled={busy || unsupported} />
		</div>
		<div class="field">
			<label class="label" for="arq-rto-max">RTO 上限 (ms)</label>
			<input id="arq-rto-max" type="number" min="1" max="65535" bind:value={rtoMax} disabled={busy || unsupported} />
		</div>
		<div class="field">
			<label class="label" for="arq-max-retx">重传次数上限</label>
			<input id="arq-max-retx" type="number" min="1" max="255" bind:value={maxRetx} disabled={busy || unsupported} />
		</div>
		<div class="field">
			<label class="label" for="arq-breaker">连续失败熔断阈值</label>
			<input id="arq-breaker" type="number" min="0" max="255" bind:value={breaker} disabled={busy || unsupported} />
		</div>
	</div>

	<div class="actions">
		<button type="button" disabled={busy || unsupported || !dirty} onclick={commit}>应用</button>
	</div>

	{#if unsupported}
		<p class="hint">当前固件不支持</p>
	{:else}
		<p class="readout">
			代数 {session.config?.arqGeneration ?? '—'}{session.config?.arqWindow == null
				? ''
				: ` · 生效窗口 ${session.config.arqWindow}`}
		</p>
		{#if readout !== null}
			<p class="readout">{readout}</p>
		{/if}
	{/if}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</div>

<style>
	.link-settings {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	.grid {
		display: grid;
		grid-template-columns: 1fr 1fr;
		gap: var(--md-sys-space-sm);
	}

	.field {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xs);
		min-width: 0;
	}

	.label {
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface);
	}

	input,
	select {
		font: inherit;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-md);
		width: 100%;
		min-width: 0;
		padding: 0.4rem 0.7rem;
		border: 1px solid var(--md-sys-color-outline-variant);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container-high);
		color: var(--md-sys-color-on-surface);
		transition:
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	input:hover:not(:disabled),
	select:hover:not(:disabled),
	input:focus-visible,
	select:focus-visible {
		border-color: var(--md-sys-color-outline);
	}

	input:disabled,
	select:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.actions {
		display: flex;
		justify-content: flex-end;
	}

	button {
		font: inherit;
		font-size: var(--md-sys-typescale-label-lg, var(--md-sys-typescale-body-md));
		padding: 0.4rem 1.1rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full, 999px);
		background: var(--md-sys-color-primary);
		color: var(--md-sys-color-on-primary);
		cursor: pointer;
		transition: opacity var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	button:disabled {
		opacity: 0.38;
		cursor: not-allowed;
	}

	.hint,
	.readout,
	.error {
		margin: 0;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-sm);
	}

	.hint,
	.readout {
		color: var(--md-sys-color-on-surface-variant);
	}

	.error {
		color: var(--md-sys-color-error);
	}
</style>
