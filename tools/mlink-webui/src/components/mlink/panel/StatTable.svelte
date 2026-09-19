<script lang="ts">
	import { MODE_NAMES, PAIR_NAMES, STAT_FIELDS, type Stats } from '../../../lib/protocol';

	let { stats }: { stats: Stats | null } = $props();

	function display(field: (typeof STAT_FIELDS)[number], value: number | undefined): string {
		if (value === undefined) return '—';
		if (field.name === 'mode') return MODE_NAMES[value] ?? '?';
		if (field.name === 'pair') return PAIR_NAMES[value] ?? '?';
		return String(value);
	}
</script>

<table>
	<thead>
		<tr>
			<th>字段</th>
			<th>说明</th>
			<th class="num">数值</th>
		</tr>
	</thead>
	<tbody>
		{#each STAT_FIELDS as field (field.tag)}
			<tr>
				<td class="key">{field.name}</td>
				<td class="label">{field.label}</td>
				<td class="num">{display(field, stats?.[field.key])}</td>
			</tr>
		{/each}
	</tbody>
</table>

<style>
	table {
		width: 100%;
		border-collapse: collapse;
		font-size: var(--md-sys-typescale-body-sm);
	}

	th {
		padding: 0 var(--md-sys-space-md) var(--md-sys-space-sm);
		text-align: left;
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	th.num {
		text-align: right;
	}

	td {
		padding: 0.35rem var(--md-sys-space-md);
		border-top: 1px solid var(--md-sys-color-outline-variant);
	}

	.key {
		font-family: var(--md-sys-typescale-font-mono);
	}

	.label {
		color: var(--md-sys-color-on-surface-variant);
	}

	.num {
		text-align: right;
		white-space: nowrap;
		font-family: var(--md-sys-typescale-font-mono);
		font-variant-numeric: tabular-nums;
	}
</style>
