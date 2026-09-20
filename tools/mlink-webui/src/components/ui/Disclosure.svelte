<script lang="ts">
	import type { Snippet } from 'svelte';

	let {
		summary,
		variant = 'plain',
		children,
	}: {
		summary: string;
		/**
		 * section：抽屉里的区块外观（底色 + 圆角 + 大写字距标题）；
		 * plain：无底色的小字标题，用于嵌在卡片/提示条里的次级折叠。
		 */
		variant?: 'plain' | 'section';
		children: Snippet;
	} = $props();
</script>

<details class="disclosure" data-variant={variant}>
	<summary>{summary}</summary>

	<div class="body">{@render children()}</div>
</details>

<style>
	.disclosure {
		/* 让子级可以拿 block-size: auto 做插值，这是内容高度动画的前提 */
		interpolate-size: allow-keywords;
	}

	.disclosure[data-variant='section'] {
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
	}

	/* 收起态：高度收到 0，content-visibility 配 allow-discrete 才能等高度走完再隐藏
	   内容（否则内容瞬间消失，动画只剩空白）。缓动走 accelerate：高度收拢是"加速
	   离场"，末尾减速会拖着不收。 */
	.disclosure::details-content {
		block-size: 0;
		overflow: hidden;
		transition:
			block-size var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate),
			content-visibility var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate) allow-discrete;
	}

	.disclosure[open]::details-content {
		block-size: auto;
		transition:
			block-size var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			content-visibility var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter) allow-discrete;
	}

	summary {
		display: flex;
		align-items: center;
		gap: var(--md-sys-space-xs);
		cursor: pointer;
		user-select: none;
		/* 原生 marker 由自绘箭头取代，才能做旋转过渡 */
		list-style: none;
	}

	.disclosure[data-variant='section'] summary {
		padding: var(--md-sys-space-sm) var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.disclosure[data-variant='section'] summary:hover {
		color: var(--md-sys-color-on-surface);
	}

	.disclosure[data-variant='plain'] summary {
		font-size: var(--md-sys-typescale-body-sm);
		opacity: 0.85;
	}

	summary::before {
		content: '▾';
		font-size: 0.8em;
		line-height: 1;
		transition: rotate var(--md-sys-motion-duration-exit) var(--md-sys-motion-easing-accelerate);
	}

	.disclosure[open] summary::before {
		transition: rotate var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter);
	}

	.disclosure:not([open]) summary::before {
		rotate: -90deg;
	}

	.body {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	.disclosure[data-variant='section'] .body {
		padding: 0 var(--md-sys-space-md) var(--md-sys-space-md);
	}
</style>
