<script lang="ts">
	import { onMount } from 'svelte';
	import type uPlot from 'uplot';
	import { MlinkSession } from '../../../lib/session.svelte';
	import { formatBytes, formatCount, formatLineCoding, formatRate, formatUartOpen, type CounterKey } from '../../../lib/rate';
	import { STANDALONE_HREF } from '../../../lib/standalone';
	import StandaloneDownloadDialog from './StandaloneDownloadDialog.svelte';
	import FirmwareUpdateDialog from './FirmwareUpdateDialog.svelte';
	import DeviceBar from './DeviceBar.svelte';
	import ConfigDrawer from '../drawer/ConfigDrawer.svelte';
	import StatusStrip from '../panel/StatusStrip.svelte';
	import StatCard from '../panel/StatCard.svelte';
	import StatTable from '../panel/StatTable.svelte';
	import RateChart from '../panel/RateChart.svelte';
	import StatusChip from '../../ui/StatusChip.svelte';
	import UsbDiagnosisAlert from './UsbDiagnosisAlert.svelte';

	const session = new MlinkSession();

	/** 单文件版本地址；null = 本构建不提供下载入口（离线页自己就是那个产物）。 */
	let { downloadHref = STANDALONE_HREF }: { downloadHref?: string | null } = $props();

	/** 配置抽屉的开关；面板本身固定在视口右侧，不参与页面流。 */
	let configOpen = $state(false);

	/** 单文件下载弹窗的开关。 */
	let downloadOpen = $state(false);

	/** 烧录窗口的开关；它盖在抽屉之上，打开时把抽屉收掉。 */
	let updateOpen = $state(false);

	function openUpdate(): void {
		configOpen = false;
		updateOpen = true;
	}

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

	/**
	 * UART 卡片副标题：桥接形态 · 串口打开状态 · 线编码。
	 * 打开状态带状态配色（已打开=ok、未打开=idle）；设备不上报的项按固定占位显示「—」。
	 */
	const uartBridge = $derived(wireless ? 'CDC ⇄ 无线桥' : 'CDC ↔ 片上 UART');
	const uartOpen = $derived(formatUartOpen(stats));
	const uartTone = $derived<'ok' | 'idle'>(stats?.uartHostOpen == null ? 'idle' : stats.uartHostOpen !== 0 ? 'ok' : 'idle');
	const uartCoding = $derived(formatLineCoding(stats));

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

	/**
	 * 分量图：DAP 与串口桥各自的有效载荷。两条上行（DAP 请求、UART 发送）与
	 * 两条下行（DAP 响应、UART 接收）方向一致，因此可以和下面那张空口总量图
	 * 逐方向对照。
	 */
	const componentLabels: string[] = ['DAP 请求', 'DAP 响应', 'UART 接收', 'UART 发送'];
	const componentData: uPlot.AlignedData = $derived(
		buildData(
			wireless
				? ['dapReqBytes', 'dapRespBytes', 'uartWlRxBytes', 'uartWlTxBytes']
				: ['dapReqBytes', 'dapRespBytes', 'uartRxBytes', 'uartTxBytes']
		)
	);

	/**
	 * 总量图：ESP-NOW 空口全量字节（含帧头与控制帧），逐方向恒 ≥ 上图的
	 * 分量之和，差额即协议开销。仅无线模式有空口流量。
	 */
	const radioLabels: string[] = ['空口发送', '空口接收'];
	const radioData: uPlot.AlignedData = $derived(buildData(['wlTxBytes', 'wlRxBytes']));
</script>

<div class="page">
	<DeviceBar {session} onconfig={() => (configOpen = true)} onupdate={openUpdate} />

	<main>
		{#if downloadHref !== null}
			<div class="standalone">
				<p>想离线用？</p>
				<button type="button" onclick={() => (downloadOpen = true)}>下载单文件HTML</button>
			</div>
		{/if}

		<UsbDiagnosisAlert {session} />

		{#if session.error !== null}
			<p class="alert" data-tone={session.state === 'waiting' ? 'busy' : 'error'}>{session.error}</p>
		{:else if session.state === 'disconnected' && session.usbDiagnosis?.status === 'ok'}
			<p class="hint">
				{#if session.devices.length > 0}
					点顶栏的「选择设备 ▾」，挑一台开始监控~
				{:else}
					点右上角「连接设备」，在选择器里选 MeguminLink（303a:4020）~
				{/if}
			</p>
		{/if}

		<StatusStrip
			{stats}
			pinging={session.manualPingInFlight}
			pingEnabled={session.device !== null && !session.commandPending}
			onping={() => void session.ping()}
		/>

		<div class="cards">
			<StatCard title="USB-DAP" subtitle="CMSIS-DAP 调试链路" rows={dapRows} />
			<StatCard title={wireless ? 'UART · Wireless' : 'UART · Local'} rows={uartRows}>
				{#snippet subtitle()}
					{uartBridge} · <StatusChip label={uartOpen} tone={uartTone} />（{uartCoding}）
				{/snippet}
			</StatCard>
			{#if wireless}
				<StatCard title="无线空口" subtitle="ESP-NOW 链路" rows={radioRows} />
			{/if}
		</div>

		<div class="charts">
			<RateChart
				title="流量 · 分量"
				subtitle="DAP 与串口，最近 60 秒"
				labels={componentLabels}
				data={componentData}
			/>
			{#if wireless}
				<RateChart
					title="流量 · 总量（无线空口）"
					subtitle="含帧头与控制帧，最近 60 秒"
					labels={radioLabels}
					data={radioData}
				/>
			{/if}
		</div>

		<section class="raw">
			<h2>原始计数器</h2>
			<StatTable {stats} />
		</section>
	</main>

	<ConfigDrawer {session} open={configOpen} onclose={() => (configOpen = false)} />

	<FirmwareUpdateDialog {session} open={updateOpen} onclose={() => (updateOpen = false)} />

	{#if downloadHref !== null}
		<StandaloneDownloadDialog
			open={downloadOpen}
			href={downloadHref}
			onclose={() => (downloadOpen = false)}
		/>
	{/if}
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

	/* 单文件下载入口：常驻在内容顶部，浅色容器衬底 + 主色描边按钮，
	   可见但不跟「连接设备」这类主操作抢视觉 */
	.standalone {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-md);
		flex-wrap: wrap;
		padding: var(--md-sys-space-sm) var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container-low);
	}

	.standalone p {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	.standalone button {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.35rem 1.1rem;
		border: 1px solid var(--md-sys-color-primary);
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
		transition: background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.standalone button:hover {
		background: var(--md-sys-color-surface-container);
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

	/* 等待设备是可自愈的中间态，用提示色而不是报错色 */
	.alert[data-tone='busy'] {
		background: var(--mlink-status-busy-container);
		color: var(--mlink-status-busy);
	}

	.hint {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}
</style>
