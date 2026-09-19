<script lang="ts">
	import { MODE_LOCAL, MODE_NAMES, MODE_WIRELESS } from '../lib/protocol';
	import type { MlinkSession } from '../lib/session.svelte';
	import SegmentedControl from './SegmentedControl.svelte';

	let { session }: { session: MlinkSession } = $props();

	/** 处于行内确认态的破坏性动作；null 表示当前没有待确认项。 */
	let confirming = $state<string | null>(null);

	const busy = $derived(session.commandPending || session.device === null);
	const mode = $derived(session.stats?.mode ?? null);
	/** 配对流程进行中（stats.pair == 2，见 protocol.ts 的 PAIR_NAMES）。 */
	const pairing = $derived(session.stats?.pair === 2);
	/** 无已配对设备：此时退出配对会被设备端立刻重新进入自动配对，置灰更诚实。 */
	const unpaired = $derived((session.config?.peerMac ?? null) === null);

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

	// 配对中：配对按钮改语义为「退出配对」；清除记录与恢复默认禁用（避免配对
	// 中途被打断），重启始终可用。触发配对在 Local 下设备端会忽略，直接置灰。
	const actions: Action[] = $derived([
		{
			key: 'pair',
			label: pairing ? '退出配对' : '触发重新配对',
			confirm: '',
			danger: false,
			disabled: pairing ? unpaired : mode !== MODE_WIRELESS,
			run: () => (pairing ? session.exitPairing() : session.enterPairing()),
		},
		{
			key: 'clear',
			label: '清除配对记录',
			confirm: '确认清除',
			danger: true,
			disabled: pairing,
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
			disabled: pairing,
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

<section class="config-actions">
	<SegmentedControl options={modes} value={mode} disabled={busy} label="链路模式" onchange={setMode} />

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
	.config-actions {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
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
