<script lang="ts">
	/** 状态色小胶囊：配对状态、对端延迟、串口打开状态共用同一套状态配色 */
	let { label, tone = 'idle' }: { label: string; tone?: 'ok' | 'busy' | 'idle' } = $props();
</script>

<span class="chip" data-tone={tone}>{label}</span>

<style>
	.chip {
		padding: 0.05rem 0.55rem;
		border-radius: var(--md-sys-shape-corner-full);
		font-size: var(--md-sys-typescale-body-md);
		font-weight: 600;
		white-space: nowrap;
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
