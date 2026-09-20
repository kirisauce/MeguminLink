<script lang="ts">
	import type { MlinkSession } from '../../../../lib/session.svelte';
	import LossSettings from './LossSettings.svelte';
	import RateSettings from './RateSettings.svelte';

	let { session }: { session: MlinkSession } = $props();
</script>

<details class="advanced">
	<summary>高级</summary>

	<div class="body">
		<RateSettings {session} />

		<LossSettings {session} />
	</div>
</details>

<style>
	.advanced {
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	summary {
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
		cursor: pointer;
		user-select: none;
	}

	summary:hover {
		color: var(--md-sys-color-on-surface);
	}

	.body {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
		padding: 0 var(--md-sys-space-md) var(--md-sys-space-md);
	}
</style>
