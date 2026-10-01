<script lang="ts">
	import type { Snippet } from 'svelte';
	import StatusChip from './StatusChip.svelte';

	/**
	 * 「触发器 + 原生 popover 菜单」的通用壳子。
	 *
	 * 开关状态完全交给原生 popover（top-layer、light dismiss、Esc 都是浏览器的事），
	 * 所以菜单不需要遮罩，也不会跟配置抽屉抢 z-index。这里只补三件事：锚点定位、
	 * 捕获阶段的 Esc（不让它冒泡去关掉下层浮层）、以及行的排布。
	 */
	let {
		label,
		variant = 'outlined',
		align = 'start',
		title = '',
		empty = '',
		tooltip = '',
		emphasis = false,
		trigger,
		items,
		onopen,
	}: {
		/** 菜单的无障碍名；没给 trigger 时它也是触发器的可见文字 */
		label: string;
		/** outlined：顶栏那种描边胶囊；plain：设备菜单那种无边框裸按钮 */
		variant?: 'outlined' | 'plain';
		/** 面板对齐触发器的那一侧 */
		align?: 'start' | 'end';
		/** 面板顶部的小标题；空则不显示 */
		title?: string;
		/** 行之前的一句说明（例如「还没有授权过的设备」）；空则不显示 */
		empty?: string;
		/** 触发器的悬停说明 */
		tooltip?: string;
		/** 触发器整体用主色：它是当前该点的那一个 */
		emphasis?: boolean;
		/** 触发器内容；不给就渲染 label */
		trigger?: Snippet;
		items: {
			key: string;
			label: string;
			/** 第二行小字（等宽字体） */
			detail?: string;
			chip?: { label: string; tone: 'ok' | 'busy' | 'idle' };
			/** 这一行是主操作 */
			accent?: boolean;
			disabled?: boolean;
			run: () => void;
		}[];
		/** 每次展开时的回调（设备菜单要顺手刷新一次授权列表） */
		onopen?: () => void;
	} = $props();

	let triggerButton = $state<HTMLButtonElement | null>(null);
	let panel = $state<HTMLDivElement | null>(null);
	let open = $state(false);

	const uid = $props.id();
	const panelId = `${uid}-menu`;
	/** 锚名每个实例一份：原生 anchor 定位靠它把面板钉在触发器上 */
	const anchorName = `--mlink-menu-${uid}`;

	function toggle(): void {
		if (open) {
			close();
			return;
		}
		onopen?.();
		open = true;
		panel?.showPopover();
	}

	function close(): void {
		open = false;
		panel?.hidePopover();
	}

	function pick(item: { run: () => void }): void {
		close();
		item.run();
	}

	function onkeydown(event: KeyboardEvent): void {
		// 捕获阶段消费 Esc：否则同一个按键会冒泡到 window，把下层浮层一起关掉
		if (!open || event.key !== 'Escape') return;
		event.preventDefault();
		event.stopPropagation();
		close();
		triggerButton?.focus();
	}

	/** 原生 popover 的开关状态是唯一事实来源：Esc、点空白、再点触发按钮都由浏览器处理 */
	function ontoggle(event: Event): void {
		if ((event as Event & { newState?: string }).newState === 'closed') open = false;
	}
</script>

<svelte:window onkeydowncapture={onkeydown} />

<button
	class="trigger"
	class:plain={variant === 'plain'}
	class:emphasis
	type="button"
	style="anchor-name: {anchorName}"
	bind:this={triggerButton}
	title={tooltip === '' ? undefined : tooltip}
	aria-expanded={open}
	aria-controls={panelId}
	onclick={toggle}
>
	{#if trigger}{@render trigger()}{:else}{label}{/if}
	<svg class="chevron" class:flipped={open} viewBox="0 0 16 16" aria-hidden="true">
		<path
			d="M4 6.5 8 10.5l4-4"
			fill="none"
			stroke="currentColor"
			stroke-width="1.6"
			stroke-linecap="round"
			stroke-linejoin="round"
		/>
	</svg>
</button>

<div
	class="menu"
	data-align={align}
	id={panelId}
	popover="auto"
	style="position-anchor: {anchorName}"
	bind:this={panel}
	ontoggle={ontoggle}
>
	{#if title !== ''}
		<p class="title">{title}</p>
	{/if}
	{#if empty !== ''}
		<p class="empty">{empty}</p>
	{/if}

	{#each items as item (item.key)}
		<button
			class="row"
			class:accent={item.accent ?? false}
			type="button"
			disabled={item.disabled ?? false}
			onclick={() => pick(item)}
		>
			{#if item.detail === undefined}
				<span class="label">{item.label}</span>
			{:else}
				<span class="main">
					<span class="label">{item.label}</span>
					<span class="detail">{item.detail}</span>
				</span>
			{/if}
			{#if item.chip !== undefined}
				<StatusChip label={item.chip.label} tone={item.chip.tone} />
			{/if}
		</button>
	{/each}
</div>

<style>
	.trigger {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-xs);
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.4rem 1.1rem;
		border: 1px solid var(--md-sys-color-outline);
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.trigger:hover {
		background: var(--md-sys-color-surface-container);
		border-color: var(--md-sys-color-primary);
	}

	/* 设备菜单那种：没有描边，贴在顶栏里当一段可点的文字 */
	.trigger.plain {
		max-width: 22rem;
		padding: 0.15rem 0.55rem;
		border-color: transparent;
		color: var(--md-sys-color-on-surface);
		font-size: var(--md-sys-typescale-body-sm);
	}

	.trigger.plain:hover {
		border-color: transparent;
	}

	/* 还没选设备时这行字就是主操作：换主色 + 加粗，别让人猜该点哪 */
	.trigger.emphasis {
		color: var(--md-sys-color-primary);
		font-weight: 600;
	}

	.chevron {
		width: 1em;
		height: 1em;
		flex: none;
		transition: transform var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	/* 展开时翻过来，收起时转回去 */
	.chevron.flipped {
		transform: rotate(180deg);
	}

	/* 设备菜单那支原本就是淡色的提示符；描边胶囊上跟着字色走 */
	.trigger.plain .chevron {
		color: var(--md-sys-color-on-surface-variant);
	}

	/* 常驻 DOM 的原生 popover：top-layer、light dismiss、Esc 都由浏览器负责 */
	.menu {
		position: fixed;
		inset: auto;
		top: anchor(bottom);
		left: anchor(left);
		position-try-fallbacks: flip-block, flip-inline;
		margin: var(--md-sys-space-xs) 0 0;
		min-width: 12rem;
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

	.menu[data-align='end'] {
		left: auto;
		right: anchor(right);
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

	.empty {
		margin: 0;
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-body-sm);
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

	.row:hover:not(:disabled) {
		background: var(--md-sys-color-surface-container);
	}

	.row:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.accent {
		color: var(--md-sys-color-primary);
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
</style>
