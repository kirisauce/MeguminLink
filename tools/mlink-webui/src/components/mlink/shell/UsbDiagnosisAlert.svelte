<script lang="ts">
	import type { MlinkSession } from '../../../lib/session.svelte';
	import Disclosure from '../../ui/Disclosure.svelte';
	import StandaloneDownloadDialog from './StandaloneDownloadDialog.svelte';

	let { session }: { session: MlinkSession } = $props();

	const diagnosis = $derived(session.usbDiagnosis);
	let downloadOpen = $state(false);
</script>

{#if diagnosis !== null && diagnosis.status !== 'ok'}
	<section class="diagnosis" data-status={diagnosis.status}>
		<div class="head">
			<p class="headline">{diagnosis.headline}</p>
			<button type="button" class="recheck" onclick={() => void session.refreshDiagnosis()}>重新检测</button>
		</div>

		{#if diagnosis.advice.length > 0}
			<ul class="advice">
				{#each diagnosis.advice as line (line)}
					<li>{line}</li>
				{/each}
			</ul>
		{/if}

		{#if diagnosis.link !== undefined}
			<button type="button" class="download" onclick={() => (downloadOpen = true)}>
				{diagnosis.link.label}
			</button>
		{/if}

		<Disclosure summary="检查详情">
			<dl>
				{#each diagnosis.checks as check (check.id)}
					<div class="row" data-state={check.state}>
						<dt>{check.label}</dt>
						<dd>{check.detail}</dd>
					</div>
				{/each}
			</dl>
		</Disclosure>
	</section>

	{#if diagnosis.link !== undefined}
		<StandaloneDownloadDialog
			open={downloadOpen}
			href={diagnosis.link.href}
			onclose={() => (downloadOpen = false)}
		/>
	{/if}
{/if}

<style>
	.diagnosis {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
		margin: 0;
		padding: var(--md-sys-space-md) var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-md);
		font-size: var(--md-sys-typescale-body-md);
	}

	/* 接口不可用是错误态；只是还没连上（未授权 / 打开失败）用提示色，不喧宾夺主 */
	.diagnosis[data-status='blocked'] {
		background: var(--md-sys-color-error-container);
		color: var(--md-sys-color-on-error-container);
	}

	.diagnosis[data-status='degraded'] {
		background: var(--mlink-status-busy-container);
		color: var(--mlink-status-busy);
	}

	.head {
		display: flex;
		align-items: baseline;
		justify-content: space-between;
		gap: var(--md-sys-space-md);
	}

	.headline {
		margin: 0;
		font-weight: 600;
	}

	.recheck {
		font: inherit;
		font-size: var(--md-sys-typescale-label-md);
		flex: none;
		padding: 0.15rem 0.7rem;
		border: 1px solid currentColor;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: inherit;
		cursor: pointer;
		opacity: 0.8;
		transition: opacity var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.recheck:hover {
		opacity: 1;
	}

	.advice {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xs);
		margin: 0;
		padding-left: 1.1rem;
		font-size: var(--md-sys-typescale-body-sm);
	}

	dl {
		display: flex;
		flex-direction: column;
		gap: 2px;
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
	}

	.row {
		display: flex;
		gap: var(--md-sys-space-sm);
	}

	dt {
		flex: 0 0 7.5rem;
		opacity: 0.75;
	}

	dd {
		margin: 0;
	}

	.row[data-state='fail'] dd {
		font-weight: 600;
	}

	.download {
		font: inherit;
		font-size: var(--md-sys-typescale-label-lg);
		align-self: flex-start;
		padding: 0.25rem 0.9rem;
		border: 1px solid currentColor;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: inherit;
		cursor: pointer;
		opacity: 0.9;
		transition: opacity var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.download:hover {
		opacity: 1;
	}
</style>
