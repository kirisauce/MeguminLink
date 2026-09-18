<script lang="ts">
	import { STATE_TEXT, type MlinkSession } from '../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	const tone = $derived(
		session.state === 'streaming'
			? 'ok'
			: session.state === 'connecting'
				? 'busy'
				: session.state === 'error' || session.state === 'unsupported'
					? 'error'
					: 'idle'
	);
</script>

<header class="bar">
	<div class="brand">
		<h1>MeguminLink</h1>
		<span class="chip" data-tone={tone}>{STATE_TEXT[session.state]}</span>
	</div>

	<div class="device">
		<span class="device-label">设备</span>
		<code>{session.deviceLabel}</code>
	</div>

	{#if session.state === 'streaming'}
		<button class="button" onclick={() => void session.close()}>断开</button>
	{:else}
		<button
			class="button filled"
			onclick={() => void session.request()}
			disabled={session.state === 'unsupported' || session.state === 'connecting'}
		>
			连接设备
		</button>
	{/if}
</header>

<style>
	.bar {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-md);
		flex-wrap: wrap;
		padding: var(--md-sys-space-lg) var(--md-sys-space-xl);
		background: var(--md-sys-color-surface-container-low);
		border-bottom: 1px solid var(--md-sys-color-outline-variant);
	}

	.brand {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-sm);
	}

	h1 {
		margin: 0;
		font-size: var(--md-sys-typescale-title-lg);
		font-weight: 600;
		letter-spacing: 0.01em;
	}

	.chip {
		padding: 0.1rem 0.6rem;
		border-radius: var(--md-sys-shape-corner-full);
		font-size: var(--md-sys-typescale-label-md);
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

	.chip[data-tone='error'] {
		background: var(--mlink-status-error-container);
		color: var(--mlink-status-error);
	}

	.device {
		display: flex;
		align-items: baseline;
		gap: var(--md-sys-space-sm);
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	.device-label {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
	}

	.device code {
		color: var(--md-sys-color-on-surface);
	}

	.button {
		margin-left: auto;
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.4rem 1.1rem;
		border-radius: var(--md-sys-shape-corner-full);
		border: 1px solid var(--md-sys-color-outline);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
	}

	.button.filled {
		border-color: transparent;
		background: var(--md-sys-color-primary);
		color: var(--md-sys-color-on-primary);
	}

	.button:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}
</style>
