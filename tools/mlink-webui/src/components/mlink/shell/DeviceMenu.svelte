<script lang="ts">
	import type { MlinkSession } from '../../../lib/session.svelte';
	import StatusChip from '../../ui/StatusChip.svelte';

	let { session }: { session: MlinkSession } = $props();

	let trigger = $state<HTMLButtonElement | null>(null);
	let panel = $state<HTMLDivElement | null>(null);
	let open = $state(false);

	// 面板标题充当无障碍名：文案只留标题这一处，不再另写一份 aria-label
	const uid = $props.id();
	const panelId = `${uid}-menu`;

	const targets = $derived(session.devices);
	/**
	 * 等待接回的设备已经不在授权列表里（拔掉了），仍要按缓存的那份条目列出来：
	 * 点它等于「立刻重试」，不必等退避到点。
	 */
	const waiting = $derived.by(() => {
		const entry = session.currentEntry;
		if (entry === null || session.device !== null) return null;
		return targets.some((item) => item.key === entry.key) ? null : entry;
	});

	/** 当前这台的状态胶囊：与顶栏那个状态 chip 同口径，只在这一行显示 */
	const currentText = $derived(
		session.state === 'streaming'
			? '监控中'
			: session.state === 'waiting'
				? '等待接回'
				: session.state === 'connecting'
					? '连接中'
					: '未连接'
	);
	const currentTone = $derived<'ok' | 'busy' | 'idle'>(
		session.state === 'streaming'
			? 'ok'
			: session.state === 'waiting' || session.state === 'connecting'
				? 'busy'
				: 'idle'
	);

	/** 目标设备当前不在手上的那一行该标什么：连接中 / 等待接回 / 已停住 */
	const waitingChip = $derived(
		session.state === 'connecting' ? '连接中' : session.state === 'error' ? '已中断' : '等待接回'
	);
	const waitingTone = $derived<'busy' | 'idle'>(
		session.state === 'connecting' || session.state === 'waiting' ? 'busy' : 'idle'
	);

	/**
	 * 触发按钮上的那行字：没有目标设备时直接写「选择设备」，让人一眼看出点哪里；
	 * 有了目标就显示设备名。
	 */
	const name = $derived(session.supported && session.state === 'disconnected' ? '选择设备' : session.deviceLabel);

	/** 打开前刷新一次授权列表：设备可能刚插上或被拔走 */
	function toggle(): void {
		if (open) {
			close();
			return;
		}
		void session.refreshDevices();
		open = true;
		panel?.showPopover();
	}

	function close(): void {
		open = false;
		panel?.hidePopover();
	}

	async function pick(key: string): Promise<void> {
		close();
		await session.selectTarget(key);
	}

	async function addDevice(): Promise<void> {
		close();
		await session.request();
	}

	function onkeydown(event: KeyboardEvent): void {
		// 捕获阶段消费 Esc：否则同一个按键会冒泡到 window，把下层的配置抽屉一起关掉
		if (!open || event.key !== 'Escape') return;
		event.preventDefault();
		event.stopPropagation();
		close();
		trigger?.focus();
	}

	/** 原生 popover 的开关状态是唯一事实来源：Esc、点空白、再点触发按钮都由浏览器处理 */
	function ontoggle(event: Event): void {
		if ((event as Event & { newState?: string }).newState === 'closed') open = false;
	}
</script>

<svelte:window onkeydowncapture={onkeydown} />

<button
	class="trigger"
	class:accent={session.state === 'disconnected'}
	type="button"
	bind:this={trigger}
	aria-expanded={open}
	aria-controls={panelId}
	title={session.deviceDetail === '' ? '选择要监控的设备' : session.deviceDetail}
	onclick={toggle}
>
	<span class="prefix">设备</span>
	<span class="name">{name}</span>
	<span class="caret" aria-hidden="true">▾</span>
</button>

<div class="menu" id={panelId} popover="auto" bind:this={panel} ontoggle={ontoggle}>
	<p class="title">已授权设备</p>

	{#each targets as entry (entry.key)}
		<button class="row" type="button" onclick={() => void pick(entry.key)}>
			<span class="main">
				<span class="label">{entry.label}</span>
				<span class="detail">{entry.detail}{entry.keyless ? ' · 无序列号' : ''}</span>
			</span>
			{#if entry.key === session.currentKey}
				<StatusChip label={currentText} tone={currentTone} />
			{/if}
		</button>
	{/each}

	{#if waiting !== null}
		<button class="row" type="button" onclick={() => void pick(waiting.key)}>
			<span class="main">
				<span class="label">{waiting.label}</span>
				<span class="detail">{waiting.detail}</span>
			</span>
			<StatusChip label={waitingChip} tone={waitingTone} />
		</button>
	{/if}

	{#if targets.length === 0 && waiting === null}
		<p class="empty">还没有授权过的设备</p>
	{/if}

	<button class="row add" type="button" onclick={() => void addDevice()}>连接其它设备…</button>
</div>

<style>
	.trigger {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-xs);
		max-width: 22rem;
		padding: 0.15rem 0.55rem;
		border: 1px solid transparent;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface);
		font: inherit;
		font-size: var(--md-sys-typescale-body-sm);
		cursor: pointer;
		/* 菜单贴着这个按钮展开 */
		anchor-name: --mlink-device-trigger;
		transition: background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.trigger:hover {
		background: var(--md-sys-color-surface-container);
	}

	.prefix {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.name {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	/* 还没选设备时这行字就是主操作：换主色 + 加粗，别让人猜该点哪 */
	.trigger.accent .name {
		color: var(--md-sys-color-primary);
		font-weight: 600;
	}

	.caret {
		font-size: 0.7em;
		color: var(--md-sys-color-on-surface-variant);
	}

	/* 常驻 DOM 的原生 popover：top-layer、light dismiss、Esc 都由浏览器负责，
	   不与配置抽屉抢 z-index */
	.menu {
		position: fixed;
		inset: auto;
		top: anchor(bottom);
		left: anchor(left);
		position-anchor: --mlink-device-trigger;
		position-try-fallbacks: flip-block, flip-inline;
		margin: var(--md-sys-space-xs) 0 0;
		min-width: 16rem;
		max-width: min(24rem, calc(100vw - 2 * var(--md-sys-space-lg)));
		padding: var(--md-sys-space-xs);
		border: 1px solid var(--md-sys-color-outline-variant);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container-high);
		color: var(--md-sys-color-on-surface);
		box-shadow: var(--md-sys-elevation-1);
		opacity: 1;
		transform: none;
		transition:
			opacity var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			transform var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			display var(--md-sys-motion-duration-exit) allow-discrete,
			overlay var(--md-sys-motion-duration-exit) allow-discrete;
	}

	.menu:not(:popover-open) {
		opacity: 0;
		transform: translateY(-0.25rem);
	}

	@starting-style {
		.menu:popover-open {
			opacity: 0;
			transform: translateY(-0.25rem);
		}
	}

	.title {
		margin: 0;
		padding: var(--md-sys-space-xs) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.row {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-sm);
		width: 100%;
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		border: none;
		border-radius: var(--md-sys-shape-corner-sm);
		background: transparent;
		color: inherit;
		font: inherit;
		text-align: left;
		cursor: pointer;
		transition: background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.row:hover {
		background: var(--md-sys-color-surface-container);
	}

	.main {
		display: flex;
		flex-direction: column;
		flex: 1;
		min-width: 0;
	}

	.label {
		font-size: var(--md-sys-typescale-body-md);
	}

	.detail {
		overflow: hidden;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-label-md);
		color: var(--md-sys-color-on-surface-variant);
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.empty {
		margin: 0;
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	.add {
		color: var(--md-sys-color-primary);
	}
</style>
