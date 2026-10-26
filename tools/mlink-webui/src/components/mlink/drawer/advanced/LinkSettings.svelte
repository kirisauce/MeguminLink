<script lang="ts">
	import {
		MEAPROTO_STATE_IN_FLIGHT_MASK,
		MEAPROTO_STATE_IN_FLIGHT_SHIFT,
		MEAPROTO_STATE_WINDOW_MASK,
		meaprotoRetxRate
	} from '../../../../lib/protocol';
	import type { LinkConfigPatch } from '../../../../lib/protocol';
	import type { MlinkSession } from '../../../../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	const busy = $derived(session.commandPending || session.device === null);
	/** 固件上报不出这组参数：既不认识 0x1B 命令，也读不到配置项。 */
	const unsupported = $derived(session.config !== null && session.config.meaprotoRtoInitMs === null);

	/** 表单绑定值（设备值每次刷新都重置）。参数是数值，undefined = 无值。 */
	let rtoInit = $state<number | undefined>(undefined);
	let rtoMax = $state<number | undefined>(undefined);

	function syncFromDevice(): void {
		const config = session.config;
		rtoInit = config?.meaprotoRtoInitMs ?? undefined;
		rtoMax = config?.meaprotoRtoMaxMs ?? undefined;
	}

	$effect(syncFromDevice);

	/**
	 * 一次提交整组参数：设备端按"只覆盖出现的字段"处理，逐项提交没有意义。
	 * 越界或非数字的输入一律忽略并回落到设备当前值，不猜意图。
	 */
	function commit(): void {
		const values = [rtoInit, rtoMax];
		if (values.some((value) => !Number.isInteger(value))) {
			syncFromDevice();
			return;
		}
		if ((rtoInit as number) < 1 || (rtoMax as number) < (rtoInit as number)) {
			syncFromDevice();
			return;
		}

		const patch: LinkConfigPatch = {
			rtoInitMs: rtoInit,
			rtoMaxMs: rtoMax
		};
		void session.setLinkConfig(patch);
	}

	const dirty = $derived(
		session.config !== null &&
			(rtoInit !== session.config.meaprotoRtoInitMs || rtoMax !== session.config.meaprotoRtoMaxMs)
	);

	/** 运行读数：状态字节拆出窗口与在途，重传率由 tx 与 retx 现算。 */
	const readout = $derived.by(() => {
		const stats = session.stats;
		if (stats?.meaprotoState == null) return null;

		const window_ = stats.meaprotoState & MEAPROTO_STATE_WINDOW_MASK;
		const inFlight = (stats.meaprotoState >> MEAPROTO_STATE_IN_FLIGHT_SHIFT) & MEAPROTO_STATE_IN_FLIGHT_MASK;
		const rate = meaprotoRetxRate(stats);

		return [
			`窗口 ${window_}`,
			`在途 ${inFlight}`,
			`重传 ${stats.meaprotoRetx ?? 0}/${stats.meaprotoTx ?? 0}（${rate === null ? '—' : `${rate.toFixed(1)}%`}）`,
			`丢弃 ${stats.meaprotoFailed ?? 0}`,
			`去重 ${stats.meaprotoDup ?? 0}`
		].join(' · ');
	});
</script>

<div class="link-settings">
	<div class="grid">
		<div class="field">
			<label class="label" for="meaproto-rto-init">初始 RTO (ms)</label>
			<input id="meaproto-rto-init" type="number" min="1" max="65535" bind:value={rtoInit} disabled={busy || unsupported} />
		</div>
		<div class="field">
			<label class="label" for="meaproto-rto-max">RTO 上限 (ms)</label>
			<input id="meaproto-rto-max" type="number" min="1" max="65535" bind:value={rtoMax} disabled={busy || unsupported} />
		</div>
	</div>

	<div class="actions">
		<button type="button" disabled={busy || unsupported || !dirty} onclick={commit}>应用</button>
	</div>

	{#if unsupported}
		<p class="hint">当前固件不支持</p>
	{:else}
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

	input {
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
	input:focus-visible {
		border-color: var(--md-sys-color-outline);
	}

	input:disabled {
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
