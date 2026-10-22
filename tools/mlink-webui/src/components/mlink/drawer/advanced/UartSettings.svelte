<script lang="ts">
	import type { MlinkSession } from '../../../../lib/session.svelte';
	import SegmentedControl from '../../../ui/SegmentedControl.svelte';

	let { session }: { session: MlinkSession } = $props();

	const busy = $derived(session.commandPending || session.device === null);
	/** 老固件不上报 0x39：既没有当前值可显示，也不认识 0x1D 命令。 */
	const unsupported = $derived(session.stats !== null && session.stats.uartReliable === null);
	/** 设备生效值：null = 还没读到快照，两段都不选中。 */
	const reliable = $derived<number | null>(session.stats?.uartReliable ?? null);

	const onOff = [
		{ value: 1, label: '启用' },
		{ value: 0, label: '停用' },
	];

	function onchange(value: number): void {
		void session.setUartReliable(value === 1);
	}
</script>

<div class="uart-settings">
	<div class="field">
		<span class="label">Reliable UART</span>
		<SegmentedControl
			options={onOff}
			value={reliable}
			disabled={busy || unsupported}
			label="Reliable UART"
			{onchange}
		/>
	</div>

	{#if unsupported}
		<p class="hint">当前固件不支持</p>
	{/if}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</div>

<style>
	.uart-settings {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	.field {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: var(--md-sys-space-md);
	}

	.label {
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface);
	}

	.hint,
	.error {
		margin: 0;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-sm);
	}

	.hint {
		color: var(--md-sys-color-on-surface-variant);
	}

	.error {
		color: var(--md-sys-color-error);
	}
</style>
