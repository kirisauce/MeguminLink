<script lang="ts">
	import { WIFI_RATE_GROUPS } from '../../../../lib/protocol';
	import type { MlinkSession } from '../../../../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	const busy = $derived(session.commandPending || session.device === null);
	const unsupported = $derived(session.config !== null && session.config.wifiRate === null);

	/** select 的绑定值（字符串形式，'' = 无值）。设备值每次刷新都重置。 */
	let selected = $state('');

	$effect(() => {
		const current = session.config?.wifiRate ?? null;
		selected = current === null ? '' : String(current);
	});

	function onchange(): void {
		if (selected === '') return;
		void session.setWifiRate(Number(selected));
	}
</script>

<section class="rate-settings">
	<div class="field">
		<label class="label" for="wifi-rate">ESP-NOW 发送速率</label>
		<select id="wifi-rate" bind:value={selected} disabled={busy || unsupported} onchange={onchange}>
			{#if selected === ''}
				<option value="">未知</option>
			{/if}
			{#each WIFI_RATE_GROUPS as group (group.group)}
				<optgroup label={group.group}>
					{#each group.rates as option (option.value)}
						<option value={String(option.value)}>{option.label}</option>
					{/each}
				</optgroup>
			{/each}
		</select>
	</div>

	{#if unsupported}
		<p class="hint">当前固件不报告速率档位，无法在此调整（可用 mlinkctl set-rate 检查）。</p>
	{/if}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</section>

<style>
	.rate-settings {
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

	select {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		width: 100%;
		padding: 0.4rem 0.7rem;
		border: 1px solid var(--md-sys-color-outline-variant);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container-high);
		color: var(--md-sys-color-on-surface);
		transition:
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	select:hover:not(:disabled),
	select:focus-visible {
		border-color: var(--md-sys-color-outline);
	}

	select:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.hint {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	.error {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-error);
	}
</style>
