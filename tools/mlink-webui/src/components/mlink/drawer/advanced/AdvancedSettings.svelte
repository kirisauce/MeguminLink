<script lang="ts">
	import type { MlinkSession } from '../../../../lib/session.svelte';
	import LossSettings from './LossSettings.svelte';
	import RateSettings from './RateSettings.svelte';

	let { session }: { session: MlinkSession } = $props();
</script>

<details class="advanced">
	<summary>高级</summary>

	<div class="body">
		<RateSettings {session} />

		<LossSettings {session} />
	</div>
</details>

<style>
	.advanced {
		/* 让子级可以拿 block-size: auto 做插值，这是内容高度动画的前提 */
		interpolate-size: allow-keywords;
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	/* 收起态：高度收到 0，content-visibility 配 allow-discrete 才能等高度
	   走完再隐藏内容（否则内容瞬间消失，动画只剩空白）。
	   缓动走 accelerate：高度收拢是"加速离场"，末尾减速会拖着不收。 */
	.advanced::details-content {
		block-size: 0;
		overflow: hidden;
		transition:
			block-size var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate),
			content-visibility var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate) allow-discrete;
	}

	.advanced[open]::details-content {
		block-size: auto;
		transition:
			block-size var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			content-visibility var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter) allow-discrete;
	}

	summary {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-xs);
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
		cursor: pointer;
		user-select: none;
		/* 原生 marker 由自绘箭头取代，才能做旋转过渡 */
		list-style: none;
	}

	summary::before {
		content: '▾';
		font-size: 0.8em;
		line-height: 1;
		transition: rotate var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate);
	}

	.advanced[open] summary::before {
		transition: rotate var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter);
	}

	.advanced:not([open]) summary::before {
		rotate: -90deg;
	}

	summary:hover {
		color: var(--md-sys-color-on-surface);
	}

	.body {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
		padding: 0 var(--md-sys-space-md) var(--md-sys-space-md);
	}
</style>
