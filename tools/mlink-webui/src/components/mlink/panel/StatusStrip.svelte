<script lang="ts">
	import { MODE_NAMES, PAIR_NAMES, type Stats } from '../../../lib/protocol';
	import { formatUptime } from '../../../lib/rate';

	let { stats }: { stats: Stats | null } = $props();

	type Tone = 'ok' | 'busy' | 'idle';

	const pairTone: Tone = $derived(stats === null ? 'idle' : stats.pair === 1 ? 'ok' : stats.pair === 2 ? 'busy' : 'idle');

	const items: { label: string; value: string; tone?: Tone }[] = $derived([
		{ label: '链路模式', value: stats === null ? '—' : (MODE_NAMES[stats.mode] ?? '?') },
		{ label: '配对状态', value: stats === null ? '—' : (PAIR_NAMES[stats.pair] ?? '?'), tone: pairTone },
		{ label: 'RSSI', value: stats === null || stats.mode !== 1 ? '—' : `${stats.rssi} dBm` },
		{ label: '开机时长', value: stats === null ? '—' : formatUptime(stats.uptimeMs) },
	]);
</script>

<ul class="strip">
	{#each items as item (item.label)}
		<li>
			<span class="label">{item.label}</span>
			{#if item.tone}
				<span class="value chip" data-tone={item.tone}>{item.value}</span>
			{:else}
				<span class="value">{item.value}</span>
			{/if}
		</li>
	{/each}
</ul>

<style>
	.strip {
		display: grid;
		grid-template-columns: repeat(auto-fit, minmax(11rem, 1fr));
		gap: var(--md-sys-space-md);
		margin: 0;
		padding: 0;
		list-style: none;
	}

	li {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xs);
		padding: var(--md-sys-space-md) var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	.label {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.value {
		font-size: var(--md-sys-typescale-title-md);
		font-weight: 600;
	}

	.chip {
		align-self: flex-start;
		padding: 0.05rem 0.55rem;
		border-radius: var(--md-sys-shape-corner-full);
		font-size: var(--md-sys-typescale-body-md);
		background: var(--mlink-status-idle-container);
		color: var(--mlink-status-idle);
	}

	.chip[data-tone='ok'] {
		background: var(--mlink-status-ok-container);
		color: var(--mlink-status-ok);
	}

	.chip[data-tone='busy'] {
		background: var(--mlink-status-busy-container);
		color: var(--mlink-status-busy);
	}
</style>
