<script lang="ts">
	import { MODE_LOCAL, MODE_NAMES, MODE_WIRELESS } from '../lib/protocol';
	import type { MlinkSession } from '../lib/session.svelte';

	let { session }: { session: MlinkSession } = $props();

	/** 处于行内确认态的破坏性动作；null 表示当前没有待确认项。 */
	let confirming = $state<string | null>(null);

	const busy = $derived(session.commandPending || session.device === null);
	const mode = $derived(session.stats?.mode ?? null);

	const modes = [
		{ value: MODE_LOCAL, label: MODE_NAMES[MODE_LOCAL] },
		{ value: MODE_WIRELESS, label: MODE_NAMES[MODE_WIRELESS] },
	];

	type Action = {
		key: string;
		label: string;
		/** 确认态下按钮上的文字，只对破坏性动作用得上。 */
		confirm: string;
		danger: boolean;
		disabled: boolean;
		run: () => Promise<void>;
	};

	// 触发配对在 Local 下设备端会忽略，直接置灰；破坏性动作走行内二次确认
	const actions: Action[] = $derived([
		{
			key: 'pair',
			label: '触发重新配对',
			confirm: '',
			danger: false,
			disabled: mode !== MODE_WIRELESS,
			run: () => session.enterPairing(),
		},
		{
			key: 'clear',
			label: '清除配对记录',
			confirm: '确认清除',
			danger: true,
			disabled: false,
			run: () => session.clearPair(),
		},
		{
			key: 'reboot',
			label: '重启设备',
			confirm: '确认重启',
			danger: true,
			disabled: false,
			run: () => session.reboot(),
		},
		{
			key: 'reset',
			label: '恢复默认',
			confirm: '确认恢复',
			danger: true,
			disabled: false,
			run: () => session.reset(),
		},
	]);

	function activate(action: Action): void {
		if (action.danger && confirming !== action.key) {
			confirming = action.key; // 破坏性动作先落到确认态，不直接下发
			return;
		}
		confirming = null;
		void action.run();
	}

	function setMode(value: number): void {
		confirming = null;
		void session.setMode(value);
	}
</script>

<section class="actions">
	<div class="modes" role="group" aria-label="链路模式">
		{#each modes as option (option.value)}
			<button
				type="button"
				class="segment"
				aria-pressed={mode === option.value}
				disabled={busy}
				onclick={() => setMode(option.value)}
			>
				{option.label}
			</button>
		{/each}
	</div>

	{#each actions as action (action.key)}
		{#if confirming === action.key}
			<div class="confirm">
				<button
					type="button"
					class="button danger filled"
					disabled={busy}
					onclick={() => activate(action)}
				>
					{action.confirm}
				</button>
				<button type="button" class="button" disabled={busy} onclick={() => (confirming = null)}>
					取消
				</button>
			</div>
		{:else}
			<button
				type="button"
				class="button"
				class:danger={action.danger}
				disabled={busy || action.disabled}
				onclick={() => activate(action)}
			>
				{action.label}
			</button>
		{/if}
	{/each}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</section>

<style>
	.actions {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	.modes {
		display: flex;
		gap: var(--md-sys-space-xs);
		padding: var(--md-sys-space-xs);
		border-radius: var(--md-sys-shape-corner-full);
		background: var(--md-sys-color-surface-container);
	}

	.segment {
		flex: 1;
		padding: 0.35rem 0.75rem;
		border: none;
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface-variant);
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		cursor: pointer;
	}

	.segment[aria-pressed='true'] {
		background: var(--md-sys-color-primary-container);
		color: var(--md-sys-color-on-primary-container);
		font-weight: 600;
	}

	.segment:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.button {
		font: inherit;
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.45rem 0.9rem;
		border: 1px solid var(--md-sys-color-outline);
		border-radius: var(--md-sys-shape-corner-full);
		background: transparent;
		color: var(--md-sys-color-on-surface);
		cursor: pointer;
	}

	.button:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.button.danger {
		border-color: var(--md-sys-color-error);
		color: var(--md-sys-color-error);
	}

	.button.danger.filled {
		border-color: transparent;
		background: var(--md-sys-color-error);
		color: var(--md-sys-color-on-error);
	}

	.confirm {
		display: flex;
		gap: var(--md-sys-space-sm);
	}

	.confirm .button:first-child {
		flex: 1;
	}

	.error {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-error);
	}
</style>
