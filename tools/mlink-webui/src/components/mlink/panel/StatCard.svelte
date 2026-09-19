<script lang="ts">
	import type { Snippet } from 'svelte';

	let {
		title,
		subtitle,
		rows,
	}: {
		title: string;
		/** 纯文本；需要内嵌状态胶囊等标记时传 snippet */
		subtitle?: string | Snippet;
		rows: { label: string; rate: string; total: string }[];
	} = $props();
</script>

<section class="card">
	<header>
		<h2>{title}</h2>
		{#if typeof subtitle === 'function'}
			<p>{@render subtitle()}</p>
		{:else if subtitle}
			<p>{subtitle}</p>
		{/if}
	</header>

	<dl>
		{#each rows as row (row.label)}
			<div class="row">
				<dt>{row.label}</dt>
				<dd>
					<span class="rate">{row.rate}</span>
					<span class="total">累计 {row.total}</span>
				</dd>
			</div>
		{/each}
	</dl>
</section>

<style>
	.card {
		padding: var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-lg);
		background: var(--md-sys-color-surface-container);
		box-shadow: var(--md-sys-elevation-1);
	}

	header {
		margin-bottom: var(--md-sys-space-md);
	}

	h2 {
		margin: 0;
		font-size: var(--md-sys-typescale-title-md);
		font-weight: 600;
	}

	header p {
		margin: var(--md-sys-space-xs) 0 0;
		font-size: var(--md-sys-typescale-label-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	dl {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
		margin: 0;
	}

	.row {
		display: grid;
		grid-template-columns: 1fr auto;
		align-items: baseline;
		gap: 0 var(--md-sys-space-md);
		padding-top: var(--md-sys-space-sm);
		border-top: 1px solid var(--md-sys-color-outline-variant);
	}

	.row:first-child {
		padding-top: 0;
		border-top: none;
	}

	dt {
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	dd {
		display: contents;
		margin: 0;
	}

	.rate {
		grid-column: 2;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-title-md);
		font-variant-numeric: tabular-nums;
		font-weight: 600;
	}

	.total {
		grid-column: 2;
		text-align: right;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-label-md);
		font-variant-numeric: tabular-nums;
		color: var(--md-sys-color-on-surface-variant);
	}
</style>
