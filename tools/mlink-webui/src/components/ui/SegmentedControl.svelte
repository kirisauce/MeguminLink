<script lang="ts" generics="T extends string | number">
	import { onMount } from 'svelte';

	type Option = { value: T; label: string };

	let {
		options,
		value,
		disabled = false,
		label,
		onchange,
	}: {
		options: Option[];
		value: T | null;
		disabled?: boolean;
		/** 分段控件的可访问名称（role="group" 的 aria-label）。 */
		label?: string;
		onchange: (value: T) => void;
	} = $props();

	/** 选中项下标；当前值不在列表里（例如设备还没回报）时为 -1，指示器隐藏。 */
	const index = $derived(options.findIndex((option) => option.value === value));

	// 首次挂载不播过渡：组件常驻 DOM，避免出现时指示器从第一项滑到选中项
	let mounted = $state(false);
	onMount(() => {
		requestAnimationFrame(() => {
			mounted = true;
		});
	});
</script>

<div
	class="segmented"
	class:animate={mounted}
	class:no-selection={index < 0}
	role="group"
	aria-label={label}
	style="--count: {options.length}; --index: {index < 0 ? 0 : index}"
>
	<span class="indicator" aria-hidden="true"></span>

	{#each options as option (option.value)}
		<button
			type="button"
			class="segment"
			aria-pressed={option.value === value}
			{disabled}
			onclick={() => onchange(option.value)}
		>
			{option.label}
		</button>
	{/each}
</div>

<style>
	.segmented {
		position: relative;
		display: flex;
		gap: var(--md-sys-space-xs);
		padding: var(--md-sys-space-xs);
		border-radius: var(--md-sys-shape-corner-full);
		background: var(--md-sys-color-surface-container);
	}

	/* 单个指示器在各段之间平移：只动一个元素，按钮本身不参与布局变化 */
	.indicator {
		position: absolute;
		top: var(--md-sys-space-xs);
		bottom: var(--md-sys-space-xs);
		left: var(--md-sys-space-xs);
		width: calc(
			(100% - 2 * var(--md-sys-space-xs) - (var(--count) - 1) * var(--md-sys-space-xs)) / var(--count)
		);
		border-radius: var(--md-sys-shape-corner-full);
		background: var(--md-sys-color-primary-container);
		transform: translateX(calc(var(--index) * (100% + var(--md-sys-space-xs))));
	}

	.animate .indicator {
		transition:
			transform var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter),
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-enter);
	}

	.no-selection .indicator {
		opacity: 0;
	}

	.segment {
		position: relative;
		z-index: 1;
		flex: 1;
		padding: 0.35rem 0.75rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface-variant);
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		cursor: pointer;
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	/* 未选中项 hover 时垫一层 state layer；选中项的底色由指示器负责，不再叠 */
	.segment:hover:not(:disabled):not([aria-pressed='true']) {
		background: color-mix(in srgb, var(--md-sys-color-on-surface) 8%, transparent);
	}

	.segment[aria-pressed='true'] {
		color: var(--md-sys-color-on-primary-container);
		font-weight: 600;
	}

	.segment:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}
</style>
