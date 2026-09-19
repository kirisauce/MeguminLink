<script lang="ts">
	let {
		open,
		title,
		message,
		confirmLabel = '确认',
		cancelLabel = '取消',
		danger = false,
		onconfirm,
		ondismiss,
	}: {
		open: boolean;
		title: string;
		message: string;
		confirmLabel?: string;
		cancelLabel?: string;
		/** 破坏性操作：确认按钮改用错误色填充。 */
		danger?: boolean;
		onconfirm: () => void;
		/** 取消：取消按钮、遮罩与 Esc 都走这里。 */
		ondismiss: () => void;
	} = $props();

	const uid = $props.id();
	const titleId = `${uid}-title`;
	const messageId = `${uid}-message`;

	let cancelButton = $state<HTMLButtonElement | null>(null);
	let confirmButton = $state<HTMLButtonElement | null>(null);

	// 打开时焦点落在「取消」：破坏性操作的默认落点应是安全的那个，误按回车
	// 不会直接执行；关闭后把焦点还给触发它的元素。
	$effect(() => {
		if (!open) return;
		const previous = document.activeElement;
		cancelButton?.focus();
		return () => {
			if (previous instanceof HTMLElement) previous.focus();
		};
	});

	function onkeydown(event: KeyboardEvent): void {
		if (!open) return;
		if (event.key === 'Escape') {
			// 捕获阶段拦下：否则同一个 Esc 会冒泡到 window，把下层的抽屉一起关掉
			event.preventDefault();
			event.stopPropagation();
			ondismiss();
			return;
		}
		if (event.key !== 'Tab' || cancelButton === null || confirmButton === null) return;
		// 两个按钮之间循环，两个方向都由本组件接管（不依赖浏览器的默认焦点行为）
		event.preventDefault();
		event.stopPropagation();
		const target = event.shiftKey
			? document.activeElement === cancelButton
				? confirmButton
				: cancelButton
			: document.activeElement === confirmButton
				? cancelButton
				: confirmButton;
		target.focus();
	}
</script>

<!-- 捕获阶段监听 window：无论焦点是否还在弹窗内都先拿到 Esc/Tab，再靠
     stopPropagation 拦住冒泡阶段的下层监听器（抽屉的 Esc 就在 window 冒泡阶段）。 -->
<svelte:window onkeydowncapture={onkeydown} />

<!-- 常驻 DOM：class 切换驱动进出 transition；关闭态由 visibility + inert 兜住交互 -->
<button
	class="dialog-scrim"
	class:open
	type="button"
	tabindex="-1"
	aria-label="关闭确认弹窗"
	onclick={ondismiss}
></button>

<div
	class="dialog"
	class:open
	class:danger
	inert={!open}
	role="dialog"
	aria-modal="true"
	aria-labelledby={titleId}
	aria-describedby={messageId}
>
	<h2 id={titleId}>{title}</h2>
	<p id={messageId}>{message}</p>

	<div class="buttons">
		<button class="button" type="button" bind:this={cancelButton} onclick={ondismiss}>
			{cancelLabel}
		</button>
		<button class="button confirm" type="button" bind:this={confirmButton} onclick={onconfirm}>
			{confirmLabel}
		</button>
	</div>
</div>

<style>
	.dialog-scrim {
		position: fixed;
		inset: 0;
		z-index: 20;
		padding: 0;
		border: none;
		background: var(--md-sys-color-scrim);
		opacity: 0;
		visibility: hidden;
		pointer-events: none;
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			visibility 0s linear var(--md-sys-motion-duration-medium);
	}

	.dialog-scrim.open {
		opacity: 1;
		visibility: visible;
		pointer-events: auto;
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			visibility 0s;
	}

	.dialog {
		position: fixed;
		top: 50%;
		left: 50%;
		z-index: 21;
		width: min(35rem, calc(100vw - 2 * var(--md-sys-space-xl)));
		padding: var(--md-sys-space-xl);
		border-radius: var(--md-sys-shape-corner-xl);
		background: var(--md-sys-color-surface-container-high);
		box-shadow: var(--md-sys-elevation-1);
		opacity: 0;
		visibility: hidden;
		transform: translate(-50%, -50%) scale(0.9);
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			transform var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			visibility 0s linear var(--md-sys-motion-duration-medium);
	}

	.dialog.open {
		opacity: 1;
		visibility: visible;
		transform: translate(-50%, -50%) scale(1);
		transition:
			opacity var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			transform var(--md-sys-motion-duration-medium) var(--md-sys-motion-easing-standard),
			visibility 0s;
	}

	h2 {
		margin: 0 0 var(--md-sys-space-md);
		font-size: var(--md-sys-typescale-headline-sm);
		font-weight: 600;
	}

	p {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	.buttons {
		display: flex;
		justify-content: flex-end;
		gap: var(--md-sys-space-sm);
		margin-top: var(--md-sys-space-xl);
	}

	.button {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.5rem 1.1rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-primary);
		cursor: pointer;
	}

	.button:hover {
		background: var(--md-sys-color-surface-container);
	}

	.confirm {
		background: var(--md-sys-color-primary);
		color: var(--md-sys-color-on-primary);
	}

	.danger .confirm {
		background: var(--md-sys-color-error);
		color: var(--md-sys-color-on-error);
	}
</style>
