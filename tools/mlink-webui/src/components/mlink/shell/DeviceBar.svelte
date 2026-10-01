<script lang="ts">
	import { STATE_TEXT, type MlinkSession } from '../../../lib/session.svelte';
	import PopoverMenu from '../../ui/PopoverMenu.svelte';
	import DeviceMenu from './DeviceMenu.svelte';

	let {
		session,
		onconfig,
		onupdate,
	}: { session: MlinkSession; onconfig: () => void; onupdate: () => void } = $props();

	const tone = $derived(
		session.state === 'streaming'
			? 'ok'
			: session.state === 'connecting' || session.state === 'waiting'
				? 'busy'
				: session.state === 'error' || session.state === 'unsupported'
					? 'error'
					: 'idle'
	);
</script>

<header class="bar">
	<div class="brand">
		<h1>MeguminLink</h1>
		<span class="chip" data-tone={tone}>{STATE_TEXT[session.state]}</span>
	</div>

	<DeviceMenu {session} />

	<div class="actions">
		<!-- 烧录不依赖当前连接：设备已经在更新模式时 WebUSB 侧根本没有它，这条入口仍要能用 -->
		<PopoverMenu
			label="操作"
			align="end"
			items={[{ key: 'update', label: '烧录固件…', run: onupdate }]}
		/>
		{#if session.state === 'streaming'}
			<button class="button" onclick={onconfig}>配置</button>
			<button class="button" onclick={() => void session.close()}>断开</button>
		{:else}
			<button
				class="button filled"
				onclick={() => void session.request()}
				disabled={session.state === 'unsupported' || session.state === 'connecting'}
			>
				连接设备
			</button>
		{/if}
	</div>
</header>

<style>
	.bar {
		position: sticky;
		top: 0;
		/* 低于配置抽屉（10/11）与确认弹窗（20/21）：滚动时顶栏贴在内容之上，
		   但抽屉打开后必须被抽屉的遮罩盖住 */
		z-index: 5;
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-md);
		flex-wrap: wrap;
		padding: var(--md-sys-space-lg) var(--md-sys-space-xl);
		/* 背景必须不透明：滚动时下方内容会从顶栏底下穿过 */
		background: var(--md-sys-color-surface-container-low);
		border-bottom: 1px solid var(--md-sys-color-outline-variant);
	}

	.brand {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-sm);
	}

	h1 {
		margin: 0;
		font-size: var(--md-sys-typescale-title-lg);
		font-weight: 600;
		letter-spacing: 0.01em;
	}

	.chip {
		padding: 0.1rem 0.6rem;
		border-radius: var(--md-sys-shape-corner-full);
		font-size: var(--md-sys-typescale-label-md);
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

	.chip[data-tone='error'] {
		background: var(--mlink-status-error-container);
		color: var(--mlink-status-error);
	}

	.actions {
		display: flex;
		gap: var(--md-sys-space-sm);
		margin-left: auto;
	}

	.button {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.4rem 1.1rem;
		border-radius: var(--md-sys-shape-corner-full);
		border: 1px solid var(--md-sys-color-outline);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.button:hover:not(:disabled) {
		background: var(--md-sys-color-surface-container);
	}

	.button.filled {
		border-color: transparent;
		background: var(--md-sys-color-primary);
		color: var(--md-sys-color-on-primary);
	}

	.button.filled:hover:not(:disabled) {
		/* 填充按钮的 hover 是叠一层 state layer，而不是换成容器色（那会让白字看不清） */
		background: color-mix(in srgb, var(--md-sys-color-primary) 92%, var(--md-sys-color-on-primary));
	}

	.button:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}
</style>
