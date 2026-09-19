<script lang="ts">
	import { MODE_NAMES, MODE_WIRELESS, PAIR_NAMES, PAIR_PAIRED, PEER_OFFLINE, PEER_ONLINE, type Stats } from '../../../lib/protocol';
	import { formatRtt, formatUptime } from '../../../lib/rate';
	import StatusChip from '../../ui/StatusChip.svelte';

	let {
		stats,
		pinging = false,
		pingEnabled = false,
		onping,
	}: {
		stats: Stats | null;
		/** 手动探测在途（等设备结算）：显示「探测中…」并禁用点击；周期探测不占用它。 */
		pinging?: boolean;
		/** 是否允许发起探测（设备已连接且无命令在途）；模式与配对状态由本组件自查。 */
		pingEnabled?: boolean;
		onping?: () => void;
	} = $props();

	type Tone = 'ok' | 'busy' | 'idle';

	const pairTone: Tone = $derived(stats === null ? 'idle' : stats.pair === 1 ? 'ok' : stats.pair === 2 ? 'busy' : 'idle');

	/**
	 * 对端存活：手动探测在途时优先显示「探测中…」，周期探测则静默——文案始终停在
	 * 最近一次结论上，不会每 3 秒闪一下。设备不上报该组字段（老固件）或当前不是
	 * 无线已配对（没有可探测的对端）时显示「—」，不把「没探测」伪装成「离线」。
	 */
	const peer: { value: string; tone: Tone } = $derived.by(() => {
		const current = stats;
		if (current === null || current.peerOnline === null) return { value: '—', tone: 'idle' };
		if (pinging) return { value: '探测中…', tone: 'busy' };
		if (current.mode !== MODE_WIRELESS || current.pair !== PAIR_PAIRED) return { value: '—', tone: 'idle' };
		if (current.peerOnline === PEER_ONLINE) return { value: formatRtt(current.pingRttUs), tone: 'ok' };
		if (current.peerOnline === PEER_OFFLINE) return { value: '离线', tone: 'idle' };
		return { value: '未探测', tone: 'idle' };
	});

	/**
	 * 可探测的前提：设备已连接、无线模式、已配对、上报了探测能力（老固件没有这组
	 * 字段，点了只会拿到 unknown-command），且当前没有命令在途。
	 */
	const pingReady = $derived(
		pingEnabled &&
			stats !== null &&
			stats.peerOnline !== null &&
			stats.mode === MODE_WIRELESS &&
			stats.pair === PAIR_PAIRED
	);

	const items: { label: string; value: string; tone?: Tone; ping?: boolean }[] = $derived([
		{ label: '链路模式', value: stats === null ? '—' : (MODE_NAMES[stats.mode] ?? '?') },
		{ label: '配对状态', value: stats === null ? '—' : (PAIR_NAMES[stats.pair] ?? '?'), tone: pairTone },
		{ label: '对端', value: peer.value, tone: peer.tone, ping: true },
		{ label: 'RSSI', value: stats === null || stats.mode !== 1 ? '—' : `${stats.rssi} dBm` },
		{ label: '开机时长', value: stats === null ? '—' : formatUptime(stats.uptimeMs) },
	]);

	let refreshButton = $state<HTMLButtonElement | null>(null);
	/** 待回位的起始角度（度）；null = 当前没有待办的回位 */
	let settleAngle = $state<number | null>(null);

	/**
	 * 读出 ↻ 当前转到哪：以动画自己的 currentTime 为准。
	 * transform 是合成器动画，computed transform 在部分情况下只给基值（identity），
	 * 拿它当起点会让回位从 0° 开始、看起来像没动。
	 */
	function currentSpinAngle(el: HTMLElement): number {
		const spin = el
			.getAnimations()
			.find((animation) => (animation as CSSAnimation).animationName?.endsWith('refresh-spin'));
		if (spin === undefined) return 0;
		const timing = spin.effect?.getTiming();
		const duration = typeof timing?.duration === 'number' && timing.duration > 0 ? timing.duration : 800;
		const elapsed = typeof spin.currentTime === 'number' ? spin.currentTime : 0;
		return ((elapsed % duration) / duration) * 360;
	}

	// 旋转刚被摘掉的那一刻取值：$effect.pre 在 DOM 更新前跑，此时 .spinning 还在、
	// 动画还没被移除，角度才读得到
	$effect.pre(() => {
		if (pinging) return;
		const el = refreshButton;
		if (el === null) return;
		const angle = currentSpinAngle(el);
		if (angle > 0) settleAngle = angle;
	});

	// DOM 更新后接管：先把起点钉在停下的角度（强制一次样式计算），再放开让 .refresh
	// 上的 transform 过渡把它平滑送回原位
	$effect(() => {
		const angle = settleAngle;
		const el = refreshButton;
		if (angle === null || el === null) return;
		settleAngle = null;
		el.style.transform = `rotate(${angle}deg)`;
		void el.offsetWidth;
		el.style.transform = '';
	});
</script>

<ul class="strip">
	{#each items as item (item.label)}
		<li>
			<span class="label">{item.label}</span>
			{#if item.ping}
				<!-- 延迟只做展示，刷新独立成一个按钮放在它右边 -->
				<span class="pair">
					<StatusChip label={item.value} tone={item.tone ?? 'idle'} />
					<button
						type="button"
						class="refresh"
						class:spinning={pinging}
						bind:this={refreshButton}
						disabled={!pingReady || pinging}
						onclick={() => onping?.()}
						title="立即探测已配对对端（ESP-NOW 往返时延）"
						aria-label="立即探测已配对对端"
					>
						↻
					</button>
				</span>
			{:else if item.tone}
				<StatusChip label={item.value} tone={item.tone} />
			{:else}
				<span class="value">{item.value}</span>
			{/if}
		</li>
	{/each}
</ul>

<style>
	.strip {
		display: grid;
		grid-template-columns: repeat(auto-fit, minmax(11rem, 1fr));
		gap: var(--md-sys-space-md);
		margin: 0;
		padding: 0;
		list-style: none;
	}

	li {
		display: flex;
		flex-direction: column;
		/* 起点对齐：状态胶囊在纵向排列里不该被拉满整行 */
		align-items: flex-start;
		gap: var(--md-sys-space-xs);
		padding: var(--md-sys-space-md) var(--md-sys-space-lg);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	.label {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.value {
		font-size: var(--md-sys-typescale-title-md);
		font-weight: 600;
	}

	/* 「对端」项：延迟是可读展示，刷新是它右边的独立按钮 */
	.pair {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-sm);
	}

	.refresh {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		line-height: 1;
		padding: 0.1rem 0.45rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface-variant);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			transform var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit);
	}

	.refresh:hover:not(:disabled) {
		background: var(--md-sys-color-surface-container-high);
		color: var(--md-sys-color-on-surface);
	}

	.refresh:disabled {
		cursor: not-allowed;
		opacity: 0.45;
	}

	/* 手动探测在途时把 ↻ 转起来。这是持续循环而不是状态切换，因此用固定周期，
	   不复用 tokens.css 里的过渡时长（那是给进场/交互反馈用的）。
	   旋转被摘掉时浏览器不会自动生成一条 transform 过渡，所以由组件在移除前读下
	   停住的角度、先钉住起点再放开，让这里的 transform 过渡接手平滑回位 */
	.refresh.spinning {
		animation: refresh-spin 0.8s linear infinite;
		opacity: 1;
		cursor: progress;
	}

	@keyframes refresh-spin {
		to {
			transform: rotate(1turn);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.refresh.spinning {
			animation: none;
		}
	}

	.refresh:focus-visible {
		outline: 2px solid var(--md-sys-color-primary);
		outline-offset: 2px;
	}
</style>
