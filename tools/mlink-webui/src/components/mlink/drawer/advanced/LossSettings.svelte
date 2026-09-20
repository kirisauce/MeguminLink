<script lang="ts">
	import { LOSS_PERCENT_MAX, LOSS_PERMILLE_PER_PERCENT, lossPercentText } from '../../../../lib/protocol';
	import type { MlinkSession } from '../../../../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	const busy = $derived(session.commandPending || session.device === null);
	/** 固件未编译丢包注入：既读不到配置项，也不认识 0x1A 命令。 */
	const unsupported = $derived(session.config !== null && session.config.lossPermille === null);

	/** 输入框的绑定值（百分比，undefined = 无值）。设备值每次刷新都重置。 */
	let percent = $state<number | undefined>(undefined);

	function syncFromDevice(): void {
		const permille = session.config?.lossPermille ?? null;
		percent = permille === null ? undefined : permille / LOSS_PERMILLE_PER_PERCENT;
	}

	$effect(syncFromDevice);

	function commit(): void {
		const parsed = Number(percent);
		// 越界、非数字、清空或多于一位小数：不猜意图，回落到设备当前值
		if (!Number.isFinite(parsed) || parsed < 0 || parsed > LOSS_PERCENT_MAX) {
			syncFromDevice();
			return;
		}
		const permille = Math.round(parsed * LOSS_PERMILLE_PER_PERCENT);
		if (Math.abs(parsed * LOSS_PERMILLE_PER_PERCENT - permille) > 1e-6) {
			syncFromDevice();
			return;
		}
		if (permille === session.config?.lossPermille) {
			syncFromDevice();
			return;
		}
		void session.setLossPermille(permille);
	}

	/** 注入计数只在有读数时显示：分母为 0 说明还没有帧过闸。 */
	const lossReadout = $derived.by(() => {
		const frames = session.stats?.lossFrames ?? null;
		if (frames === null || frames === 0) return null;
		const dropped = session.stats?.lossDropped ?? 0;
		return `累计丢弃 ${dropped} / 判定 ${frames} 帧`;
	});
</script>

<div class="loss-settings">
	<div class="field">
		<label class="label" for="loss-percent">丢包注入</label>
		<div class="input-row">
			<input
				id="loss-percent"
				type="number"
				min="0"
				max={LOSS_PERCENT_MAX}
				step="0.1"
				inputmode="decimal"
				bind:value={percent}
				disabled={busy || unsupported}
				onchange={commit}
			/>
			<span class="unit">%</span>
		</div>
	</div>

	{#if unsupported}
		<p class="hint">当前固件不支持</p>
	{:else if session.config !== null && session.config.lossPermille !== null}
		<p class="readout">
			当前 {lossPercentText(session.config.lossPermille)}{lossReadout === null ? '' : ` · ${lossReadout}`}
		</p>
	{/if}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</div>

<style>
	.loss-settings {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	.field {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xs);
	}

	.label {
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface);
	}

	.input-row {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-xs);
	}

	input {
		font: inherit;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-md);
		flex: 1;
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

	.unit {
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	.hint,
	.readout,
	.error {
		margin: 0;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-sm);
	}

	.hint {
		color: var(--md-sys-color-on-surface-variant);
	}

	.readout {
		color: var(--md-sys-color-on-surface-variant);
	}

	.error {
		color: var(--md-sys-color-error);
	}
</style>
