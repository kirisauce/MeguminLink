<script lang="ts">
	/**
	 * 一张滚动速率曲线。x 轴是主机时间（相对当前时刻的秒数），y 轴固定 B/s，
	 * 因此滚动窗口内的历史点不会被单位换算 reinterpret。
	 * uPlot 的 stroke 吃不了 CSS 变量，颜色在 build() 时从 token 读实际色值，
	 * 系统明暗切换时整体重建。
	 */
	import { onMount } from 'svelte';
	import uPlot from 'uplot';
	import 'uplot/dist/uPlot.min.css';
	import { formatCompact, formatRate } from '../../../lib/rate';

	let {
		title,
		subtitle = '',
		labels,
		data,
		height = 180,
	}: {
		title: string;
		subtitle?: string;
		labels: string[];
		data: uPlot.AlignedData;
		height?: number;
	} = $props();

	let host: HTMLDivElement;
	let plot: uPlot | null = null;
	/** 当前建图所用的序列签名，序列集合变化（例如 mode 切换）时需要重建。 */
	let seriesKey = '';

	function token(name: string, fallback: string): string {
		const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
		return value || fallback;
	}

	function plotWidth(): number {
		return Math.max(host?.clientWidth ?? 0, 240);
	}

	/** 取一列里最后一个有效值。没有 cursor 时 uPlot 传 null，用它回退到当前值。 */
	function lastValue(column: ArrayLike<number | null> | undefined): number | null {
		if (column === undefined) return null;
		for (let index = column.length - 1; index >= 0; index -= 1) {
			const value = column[index];
			if (value != null) return value;
		}
		return null;
	}

	function build(): void {
		if (!host) return;
		plot?.destroy();

		const colors = labels.map((_, index) => token(`--mlink-chart-${(index % 4) + 1}`, '#888888'));
		const grid = token('--md-sys-color-outline-variant', '#cccccc');
		const ink = token('--md-sys-color-on-surface-variant', '#666666');

		plot = new uPlot(
			{
				width: plotWidth(),
				height,
				pxAlign: false,
				legend: { show: true, live: true },
				cursor: { y: false, points: { size: 6 } },
				scales: { x: { time: false }, y: { auto: true } },
				axes: [
					{
						stroke: ink,
						grid: { stroke: grid, width: 1 },
						ticks: { stroke: grid, width: 1 },
						values: (u, splits) => {
							const xs = u.data[0];
							const latest = xs !== undefined && xs.length > 0 ? xs[xs.length - 1] : 0;
							return splits.map((value) => `${Math.round(value - latest)}s`);
						},
					},
					{
						stroke: ink,
						grid: { stroke: grid, width: 1 },
						ticks: { stroke: grid, width: 1 },
						size: 58,
						values: (_u, splits) => splits.map((value) => formatCompact(value)),
					},
				],
				series: [
					{
						// legend 第一列：x 值显示为相对当前时刻的秒数，与 x 轴刻度同一口径。
						value: (u: uPlot, value: number) => {
							const latest = lastValue(u.data[0]);
							const shown = value ?? latest;
							if (shown == null || latest == null) return '—';
							return `${(shown - latest).toFixed(1)}s`;
						},
					},
					...labels.map((label, index) => ({
						label,
						stroke: colors[index],
						width: 1.5,
						points: { show: false },
						// 悬停时给该点速率，未悬停时回退到最后一点的速率。
						value: (u: uPlot, value: number, seriesIdx: number) => {
							const shown = value ?? lastValue(u.data[seriesIdx]);
							return shown == null ? '—' : formatRate(shown);
						},
					})),
				],
			},
			data,
			host
		);
		seriesKey = labels.join('|');
	}

	onMount(() => {
		const observer = new ResizeObserver(() => {
			if (plot !== null) plot.setSize({ width: plotWidth(), height });
		});
		observer.observe(host);

		const media = window.matchMedia('(prefers-color-scheme: dark)');
		const onSchemeChange = () => build();
		media.addEventListener('change', onSchemeChange);

		return () => {
			observer.disconnect();
			media.removeEventListener('change', onSchemeChange);
			plot?.destroy();
			plot = null;
		};
	});

	$effect(() => {
		const key = labels.join('|');
		if (plot === null || key !== seriesKey) {
			build();
			return;
		}
		// 必须重置 scale：滚动窗口要让 x 轴跟着最新时间走，否则数据点会落在
		// 建图时定下的旧范围之外而画不出来。
		plot.setData(data);
	});
</script>

<section class="chart">
	<header>
		<h2>{title}</h2>
		{#if subtitle}
			<p>{subtitle}</p>
		{/if}
	</header>
	<div class="plot" bind:this={host}></div>
</section>

<style>
	.chart {
		padding: var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-lg);
		background: var(--md-sys-color-surface-container);
		box-shadow: var(--md-sys-elevation-1);
	}

	header {
		margin-bottom: var(--md-sys-space-sm);
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

	.plot {
		width: 100%;
	}

	:global(.uplot) {
		font-family: var(--md-sys-typescale-font-mono);
		font-size: 0.6875rem;
	}

	:global(.u-legend) {
		color: var(--md-sys-color-on-surface-variant);
	}

	:global(.u-legend .u-value) {
		color: var(--md-sys-color-on-surface);
	}
</style>
