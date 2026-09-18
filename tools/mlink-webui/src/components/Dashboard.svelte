<script lang="ts">
	import { onMount } from 'svelte';
	import type uPlot from 'uplot';
	import { MlinkSession } from '../lib/session.svelte';
	import { formatBytes, formatCount, formatRate, type CounterKey } from '../lib/rate';
	import DeviceBar from './DeviceBar.svelte';
	import StatusStrip from './StatusStrip.svelte';
	import StatCard from './StatCard.svelte';
	import StatTable from './StatTable.svelte';
	import RateChart from './RateChart.svelte';

	const session = new MlinkSession();

	onMount(() => {
		void session.restore();
		return () => session.destroy();
	});

	type CardRow = { label: string; rate: string; total: string };

	const stats = $derived(session.stats);
	const rates = $derived(session.rates);
	const wireless = $derived(stats?.mode === 1);

	const byteRate = (key: CounterKey) => (rates === null ? '—' : formatRate(rates[key]));
	const byteTotal = (key: CounterKey) => (stats === null ? '—' : formatBytes(stats[key]));
	const commandRate = (key: CounterKey) => (rates === null ? '—' : `${formatCount(rates[key])}/s`);
	const commandTotal = (key: CounterKey) => (stats === null ? '—' : formatCount(stats[key]));

	const dapRows: CardRow[] = $derived([
		{ label: '请求', rate: byteRate('dapReqBytes'), total: byteTotal('dapReqBytes') },
		{ label: '响应', rate: byteRate('dapRespBytes'), total: byteTotal('dapRespBytes') },
		{ label: '命令', rate: commandRate('dapCmds'), total: commandTotal('dapCmds') },
	]);

	const uartRows: CardRow[] = $derived(
		wireless
			? [
					{ label: '接收', rate: byteRate('uartWlRxBytes'), total: byteTotal('uartWlRxBytes') },
					{ label: '发送', rate: byteRate('uartWlTxBytes'), total: byteTotal('uartWlTxBytes') },
				]
			: [
					{ label: '接收', rate: byteRate('uartRxBytes'), total: byteTotal('uartRxBytes') },
					{ label: '发送', rate: byteRate('uartTxBytes'), total: byteTotal('uartTxBytes') },
				]
	);

	const radioRows: CardRow[] = $derived([
		{ label: '发送', rate: byteRate('wlTxBytes'), total: byteTotal('wlTxBytes') },
		{ label: '接收', rate: byteRate('wlRxBytes'), total: byteTotal('wlRxBytes') },
	]);

	/** 把滚动窗口内的采样点抽成 uPlot 的列式数据；无效点保持 null，曲线在该处断开。 */
	function buildData(keys: CounterKey[]): uPlot.AlignedData {
		const xs: number[] = [];
		const columns = keys.map(() => [] as (number | null)[]);
		for (const sample of session.history) {
			xs.push(sample.t / 1000);
			keys.forEach((key, index) => {
				columns[index].push(sample.rates === null ? null : sample.rates[key]);
			});
		}
		return [xs, ...columns] as uPlot.AlignedData;
	}

	const dapData: uPlot.AlignedData = $derived(buildData(['dapReqBytes', 'dapRespBytes']));

	const uartLabels: string[] = $derived(
		wireless ? ['接收 (CDC)', '发送 (CDC)', '空口发送', '空口接收'] : ['接收', '发送']
	);
	const uartData: uPlot.AlignedData = $derived(
		buildData(
			wireless
				? ['uartWlRxBytes', 'uartWlTxBytes', 'wlTxBytes', 'wlRxBytes']
				: ['uartRxBytes', 'uartTxBytes']
		)
	);
</script>

<div class="page">
	<DeviceBar {session} />

	<main>
		{#if session.state === 'unsupported'}
			<p class="alert">当前浏览器没有 WebUSB 接口，无法直连设备。请改用 Chrome 或 Edge 打开本页。</p>
		{:else if session.error !== null}
			<p class="alert">{session.error}</p>
		{:else if session.state === 'disconnected'}
			<p class="hint">点右上角「连接设备」，在选择器里选 MeguminLink（303a:4020）即可开始监控。</p>
		{/if}

		<StatusStrip {stats} />

		<div class="cards">
			<StatCard title="USB-DAP" subtitle="CMSIS-DAP 调试链路" rows={dapRows} />
			<StatCard
				title={wireless ? 'UART · Wireless' : 'UART · Local'}
				subtitle={wireless ? 'CDC ⇄ 无线桥' : 'CDC ↔ 片上 UART'}
				rows={uartRows}
			/>
			{#if wireless}
				<StatCard title="无线空口" subtitle="ESP-NOW 链路" rows={radioRows} />
			{/if}
		</div>

		<div class="charts">
			<RateChart
				title="USB-DAP 速率"
				subtitle="最近 60 秒，纵轴固定 B/s"
				labels={['请求', '响应']}
				data={dapData}
			/>
			<RateChart
				title={wireless ? 'UART 速率 · Wireless' : 'UART 速率 · Local'}
				subtitle={wireless ? 'CDC ⇄ 无线桥与空口，最近 60 秒' : 'CDC ↔ 片上 UART，最近 60 秒'}
				labels={uartLabels}
				data={uartData}
			/>
		</div>

		<section class="raw">
			<h2>原始计数器</h2>
			<StatTable {stats} />
		</section>
	</main>
</div>

<style>
	main {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-xl);
		max-width: 64rem;
		margin: 0 auto;
		padding: var(--md-sys-space-xl);
	}

	.cards {
		display: grid;
		grid-template-columns: repeat(auto-fit, minmax(17rem, 1fr));
		gap: var(--md-sys-space-lg);
	}

	.charts {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-lg);
	}

	.raw h2 {
		margin: 0 0 var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-title-md);
		font-weight: 600;
	}

	.alert {
		margin: 0;
		padding: var(--md-sys-space-md) var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-error-container);
		color: var(--md-sys-color-on-error-container);
		font-size: var(--md-sys-typescale-body-md);
	}

	.hint {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}
</style>
