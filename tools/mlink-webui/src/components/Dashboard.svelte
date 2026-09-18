<script lang="ts">
	import { onMount } from 'svelte';
	import { MlinkSession, STATE_TEXT } from '../lib/session.svelte';
	import { MODE_NAMES, PAIR_NAMES, STAT_FIELDS } from '../lib/protocol';

	const session = new MlinkSession();

	onMount(() => {
		void session.restore();
		return () => session.destroy();
	});

	function cliValue(name: string, value: number): string {
		if (name === 'mode') return MODE_NAMES[value] ?? '?';
		if (name === 'pair') return PAIR_NAMES[value] ?? '?';
		return String(value);
	}
</script>

<main>
	<header>
		<h1>MeguminLink</h1>
		<span class="state" data-state={session.state}>{STATE_TEXT[session.state]}</span>
		<span class="serial">设备 {session.deviceLabel}</span>
		{#if session.state === 'streaming'}
			<button onclick={() => void session.close()}>断开</button>
		{:else}
			<button
				onclick={() => void session.request()}
				disabled={session.state === 'unsupported' || session.state === 'connecting'}
			>
				连接设备
			</button>
		{/if}
	</header>

	{#if session.state === 'unsupported'}
		<p class="notice">
			当前浏览器没有 WebUSB 接口，无法直连设备。请改用 Chrome 或 Edge 打开本页。
		</p>
	{/if}

	{#if session.error !== null}
		<p class="notice error">{session.error}</p>
	{/if}

	{#if session.stats !== null}
		{@const stats = session.stats}
		<pre class="raw">{STAT_FIELDS.map((field) => `${field.name}=${cliValue(field.name, stats[field.key])}`).join(' ')}</pre>

		<table>
			<thead>
				<tr><th>字段</th><th>值</th><th>说明</th></tr>
			</thead>
			<tbody>
				{#each STAT_FIELDS as field (field.tag)}
					<tr>
						<td class="key">{field.name}</td>
						<td class="value">{cliValue(field.name, stats[field.key])}</td>
						<td class="label">{field.label}</td>
					</tr>
				{/each}
			</tbody>
		</table>
	{:else}
		<p class="notice">尚未读到数据。连接设备后这里显示与 mlinkctl 逐字段对齐的统计快照。</p>
	{/if}
</main>

<style>
	main {
		max-width: 52rem;
		margin: 0 auto;
		padding: 1.5rem 1rem 3rem;
	}

	header {
		display: flex;
		align-items: center;
		gap: 0.75rem;
		flex-wrap: wrap;
		border-bottom: 1px solid #d4d4d8;
		padding-bottom: 0.75rem;
	}

	h1 {
		font-size: 1.25rem;
		margin: 0;
		letter-spacing: 0.02em;
	}

	.state {
		font-size: 0.85rem;
		padding: 0.15rem 0.5rem;
		border-radius: 999px;
		background: #e4e4e7;
		color: #3f3f46;
	}

	.state[data-state='streaming'] {
		background: #d7f0dd;
		color: #14532d;
	}

	.state[data-state='error'],
	.state[data-state='unsupported'] {
		background: #fadadd;
		color: #7f1d1d;
	}

	.serial {
		font-family: ui-monospace, monospace;
		font-size: 0.8rem;
		color: #52525b;
	}

	button {
		margin-left: auto;
		font: inherit;
		padding: 0.35rem 0.9rem;
		border-radius: 0.35rem;
		border: 1px solid #a1a1aa;
		background: #fafafa;
		cursor: pointer;
	}

	button:disabled {
		opacity: 0.5;
		cursor: not-allowed;
	}

	.notice {
		margin: 1rem 0 0;
		font-size: 0.9rem;
		color: #52525b;
	}

	.notice.error {
		color: #b91c1c;
	}

	.raw {
		margin: 1rem 0;
		padding: 0.65rem 0.75rem;
		background: #f4f4f5;
		border-radius: 0.35rem;
		font-size: 0.78rem;
		white-space: pre-wrap;
		word-break: break-all;
	}

	table {
		width: 100%;
		border-collapse: collapse;
		font-size: 0.85rem;
	}

	th,
	td {
		text-align: left;
		padding: 0.3rem 0.5rem;
		border-bottom: 1px solid #e4e4e7;
	}

	th {
		font-weight: 600;
		color: #52525b;
	}

	.key,
	.value {
		font-family: ui-monospace, monospace;
	}

	.value {
		text-align: right;
		width: 6rem;
	}

	.label {
		color: #71717a;
	}
</style>
