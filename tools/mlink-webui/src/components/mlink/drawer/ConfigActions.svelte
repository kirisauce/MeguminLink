<script lang="ts">
	import { MODE_LOCAL, MODE_NAMES, MODE_WIRELESS } from '../../../lib/protocol';
	import type { MlinkSession } from '../../../lib/session.svelte';
	import ConfirmDialog from '../../ui/ConfirmDialog.svelte';
	import SegmentedControl from '../../ui/SegmentedControl.svelte';

	let { session, onclose = () => {} }: { session: MlinkSession; onclose?: () => void } = $props();

	/** 挂起期间提示条上的说明：进更新模式后设备不再响应 vendor 请求，页面只能等用户动手。 */
	const UPDATE_MODE_NOTICE = '设备已进入更新模式，请使用「操作 → 烧录固件…」或手动重连设备';

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

	type Confirmation = { title: string; message: string; confirmLabel: string };
	/** 弹窗内容 + 配色：关闭动画期间仍需显示，因此与「待确认哪个动作」分开存。 */
	type DialogContent = Confirmation & { danger: boolean };

	type Action = {
		key: string;
		label: string;
		danger: boolean;
		disabled: boolean;
		/** 需要二次确认的动作给出弹窗文案；为 null 表示点了直接下发。 */
		confirm: Confirmation | null;
		run: () => Promise<void>;
	};

	// 配对中：配对按钮改语义为「退出配对」；清除记录与恢复默认禁用（避免配对
	// 中途被打断），重启始终可用。触发配对在 Local 下设备端会忽略，直接置灰。
	const actions: Action[] = $derived([
		{
			key: 'pair',
			label: pairing ? '退出配对' : '触发重新配对',
			danger: false,
			disabled: pairing ? unpaired : mode !== MODE_WIRELESS,
			confirm: null,
			run: () => (pairing ? session.exitPairing() : session.enterPairing()),
		},
		{
			key: 'clear',
			label: '清除配对记录',
			danger: true,
			disabled: pairing,
			confirm: {
				title: '清除配对记录？',
				message: '设备会删掉已保存的对端 MAC，随后重新进入配对流程。',
				confirmLabel: '清除',
			},
			run: () => session.clearPair(),
		},
		{
			key: 'reboot',
			label: '重启设备',
			danger: true,
			disabled: false,
			confirm: {
				title: '重启设备？',
				message: '设备会立即重启，页面短暂断开后自动接回。',
				confirmLabel: '重启',
			},
			run: () => session.reboot(),
		},
		{
			key: 'reset',
			label: '恢复出厂设置',
			danger: true,
			disabled: pairing,
			confirm: {
				title: '恢复出厂设置？',
				message: '设备会清除配对记录、运行模式与 USB 能力配置，并自动重启设备。',
				confirmLabel: '恢复',
			},
			run: () => session.reset(),
		},
		{
			key: 'update',
			label: '进入更新模式',
			danger: true,
			disabled: false,
			confirm: {
				title: '进入更新模式？',
				message: '设备会重启进更新模式：USB 只剩一个升级串口，页面在这期间看不到它。要回正常模式，按复位键或重新插拔一次。',
				confirmLabel: '进入',
			},
			run: enterUpdateMode,
		},
	]);

	/** 待确认动作的 key（actions 每次重算都会重建对象，只能靠 key 找回）；null = 弹窗关闭。 */
	let pendingKey = $state<string | null>(null);
	/** 弹窗内容缓存：关闭时不清空，否则退场动画期间标题正文会是空的；下次打开覆盖。 */
	let confirmation = $state<DialogContent | null>(null);

	function activate(action: Action): void {
		if (action.confirm === null) {
			void action.run();
			return;
		}
		confirmation = { ...action.confirm, danger: action.danger };
		pendingKey = action.key;
	}

	function confirmPending(): void {
		const action = actions.find((item) => item.key === pendingKey);
		pendingKey = null;
		if (action !== undefined) void action.run();
	}

	function dismiss(): void {
		pendingKey = null;
	}

	function setMode(value: number): void {
		dismiss();
		void session.setMode(value);
	}

	/**
	 * 命令被设备接受才挂起会话：更新模式对 vendor 请求一律 STALL，让轮询继续跑
	 * 只会把 STALL 当成故障反复刷提示。这里不接管串口，回正常模式由用户显式重连。
	 *
	 * 顺手收起抽屉：设备已经不在，面板上的字段与按钮只会剩一片「—」和禁用态，
	 * 让位给面板上那句提示更有用。
	 */
	async function enterUpdateMode(): Promise<void> {
		await session.enterUpdateMode();
		if (session.commandError !== null) return;
		session.suspend(UPDATE_MODE_NOTICE);
		onclose();
	}
</script>

<section class="config-actions">
	<SegmentedControl options={modes} value={mode} disabled={busy} label="链路模式" onchange={setMode} />

	{#each actions as action (action.key)}
		<button
			type="button"
			class="button"
			class:danger={action.danger}
			disabled={busy || action.disabled}
			onclick={() => activate(action)}
		>
			{action.label}
		</button>
	{/each}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}
</section>

<ConfirmDialog
	open={pendingKey !== null}
	title={confirmation?.title ?? ''}
	message={confirmation?.message ?? ''}
	confirmLabel={confirmation?.confirmLabel ?? '确认'}
	danger={confirmation?.danger ?? false}
	onconfirm={confirmPending}
	ondismiss={dismiss}
/>

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
		transition:
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	.button:hover:not(:disabled) {
		background: var(--md-sys-color-surface-container);
	}

	.button:disabled {
		opacity: 0.45;
		cursor: not-allowed;
	}

	.button.danger {
		border-color: var(--md-sys-color-error);
		color: var(--md-sys-color-error);
	}

	.button.danger:hover:not(:disabled) {
		background: var(--md-sys-color-error-container);
	}

	.error {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-error);
	}
</style>
