<script lang="ts">
	import { MODE_NAMES, PAIR_NAMES } from '../../../lib/protocol';
	import type { MlinkSession } from '../../../lib/session.svelte';
	import AdvancedSettings from './advanced/AdvancedSettings.svelte';
	import ConfigActions from './ConfigActions.svelte';
	import UsbSettings from './UsbSettings.svelte';

	let { session, open, onclose }: { session: MlinkSession; open: boolean; onclose: () => void } = $props();

	// 面板标题充当无障碍名：文案只留标题这一处，不再另写一份 aria-label
	const uid = $props.id();
	const titleId = `${uid}-title`;

	let panel = $state<HTMLElement | null>(null);

	// 身份、配置与 USB 能力都是低频变化：打开时刷新一次即可，不跟随 200ms 轮询
	$effect(() => {
		if (open) {
			void session.refreshConfig();
			void session.refreshUsbConfig();
		}
	});

	// 打开后把焦点移进面板，键盘 Tab 从抽屉开始、Esc 随时可关
	$effect(() => {
		if (open) panel?.focus();
	});

	const config = $derived(session.config);
	const stats = $derived(session.stats);

	const rows: { label: string; value: string }[] = $derived([
		{ label: '固件版本', value: config?.fwVersion || '—' },
		{ label: '芯片', value: config?.chipTarget || '—' },
		{ label: 'IDF 版本', value: config?.idfVersion || '—' },
		{ label: '链路模式', value: stats === null ? '—' : (MODE_NAMES[stats.mode] ?? '?') },
		{ label: '配对状态', value: stats === null ? '—' : (PAIR_NAMES[stats.pair] ?? '?') },
		{ label: '对端 MAC', value: config?.peerMac ?? '—' },
	]);

	function onkeydown(event: KeyboardEvent): void {
		if (event.key === 'Escape' && open) onclose();
	}
</script>

<svelte:window onkeydown={onkeydown} />

<!-- 常驻 DOM：class 切换驱动进出 transition，关闭态由 visibility + inert 兜住交互 -->
<button class="scrim" class:open type="button" tabindex="-1" aria-label="关闭配置面板" onclick={onclose}></button>

<div
	class="drawer"
	class:open
	inert={!open}
	role="dialog"
	aria-modal="true"
	aria-labelledby={titleId}
	tabindex="-1"
	bind:this={panel}
>
	<header>
		<h2 id={titleId}>设备配置</h2>
		<button class="close" type="button" aria-label="关闭" onclick={onclose}>✕</button>
	</header>

	<dl>
		{#each rows as row (row.label)}
			<div class="row">
				<dt>{row.label}</dt>
				<dd>{row.value}</dd>
			</div>
		{/each}
	</dl>

	<UsbSettings {session} />

	<AdvancedSettings {session} />

	<ConfigActions {session} onclose={onclose} />
</div>

<style>
	.scrim {
		position: fixed;
		inset: 0;
		z-index: 10;
		padding: 0;
		border: none;
		background: var(--md-sys-color-scrim);
		opacity: 0;
		visibility: hidden;
		pointer-events: none;
		transition:
			opacity var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit),
			visibility 0s linear var(--md-sys-motion-duration-exit);
	}

	.scrim.open {
		opacity: 1;
		visibility: visible;
		pointer-events: auto;
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			visibility 0s;
	}

	.drawer {
		position: fixed;
		top: 0;
		right: 0;
		bottom: 0;
		z-index: 11;
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-lg);
		width: min(22rem, 100vw);
		padding: var(--md-sys-space-lg) var(--md-sys-space-xl) var(--md-sys-space-xl);
		background: var(--md-sys-color-surface-container-high);
		box-shadow: var(--md-sys-elevation-1);
		overflow-y: auto;
		transform: translateX(100%);
		visibility: hidden;
		transition:
			transform var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-exit),
			visibility 0s linear var(--md-sys-motion-duration-exit);
	}

	.drawer.open {
		transform: none;
		visibility: visible;
		transition:
			transform var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			visibility 0s;
	}

	.drawer:focus-visible {
		outline: none;
	}

	header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: var(--md-sys-space-sm);
	}

	h2 {
		margin: 0;
		font-size: var(--md-sys-typescale-title-md);
		font-weight: 600;
	}

	.close {
		font: inherit;
		line-height: 1;
		padding: 0.35rem 0.55rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface-variant);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.close:hover {
		background: var(--md-sys-color-surface-container);
	}

	dl {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
		margin: 0;
	}

	.row {
		display: flex;
		align-items: baseline;
		justify-content: space-between;
		gap: var(--md-sys-space-md);
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	dt {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	dd {
		margin: 0;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-md);
		text-align: right;
		word-break: break-all;
	}
</style>
