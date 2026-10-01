<script lang="ts">
	import type { MlinkSession } from '../../../lib/session.svelte';
	import PopoverMenu from '../../ui/PopoverMenu.svelte';

	let { session }: { session: MlinkSession } = $props();

	const targets = $derived(session.devices);
	/**
	 * 等待接回的设备已经不在授权列表里（拔掉了），仍要按缓存的那份条目列出来：
	 * 点它等于「立刻重试」，不必等退避到点。
	 */
	const waiting = $derived.by(() => {
		const entry = session.currentEntry;
		if (entry === null || session.device !== null) return null;
		return targets.some((item) => item.key === entry.key) ? null : entry;
	});

	/** 当前这台的状态胶囊：与顶栏那个状态 chip 同口径，只在这一行显示 */
	const currentText = $derived(
		session.state === 'streaming'
			? '监控中'
			: session.state === 'waiting'
				? '等待接回'
				: session.state === 'connecting'
					? '连接中'
					: '未连接'
	);
	const currentTone = $derived<'ok' | 'busy' | 'idle'>(
		session.state === 'streaming'
			? 'ok'
			: session.state === 'waiting' || session.state === 'connecting'
				? 'busy'
				: 'idle'
	);

	/** 目标设备当前不在手上的那一行该标什么：连接中 / 等待接回 / 已停住 */
	const waitingChip = $derived(
		session.state === 'connecting' ? '连接中' : session.state === 'error' ? '已中断' : '等待接回'
	);
	const waitingTone = $derived<'busy' | 'idle'>(
		session.state === 'connecting' || session.state === 'waiting' ? 'busy' : 'idle'
	);

	/**
	 * 触发按钮上的那行字：没有目标设备时直接写「选择设备」，让人一眼看出点哪里；
	 * 有了目标就显示设备名。
	 */
	const name = $derived(session.supported && session.state === 'disconnected' ? '选择设备' : session.deviceLabel);

	/** 菜单里的行：已授权设备、等待接回的那台、以及「连接其它设备…」 */
	const items = $derived([
		...targets.map((entry) => ({
			key: entry.key,
			label: entry.label,
			detail: entry.keyless ? `${entry.detail} · 无序列号` : entry.detail,
			chip: entry.key === session.currentKey ? { label: currentText, tone: currentTone } : undefined,
			run: () => void session.selectTarget(entry.key),
		})),
		...(waiting === null
			? []
			: [
					{
						key: waiting.key,
						label: waiting.label,
						detail: waiting.detail,
						chip: { label: waitingChip, tone: waitingTone },
						run: () => void session.selectTarget(waiting.key),
					},
				]),
		{ key: 'add', label: '连接其它设备…', accent: true, run: () => void session.request() },
	]);
</script>

<PopoverMenu
	label="设备"
	title="已授权设备"
	empty={targets.length === 0 && waiting === null ? '还没有授权过的设备' : ''}
	variant="plain"
	emphasis={session.supported && session.state === 'disconnected'}
	tooltip={session.deviceDetail === '' ? '选择要监控的设备' : session.deviceDetail}
	{items}
	onopen={() => void session.refreshDevices()}
>
	{#snippet trigger()}
		<span class="prefix">设备</span>
		<span class="name">{name}</span>
	{/snippet}
</PopoverMenu>

<style>
	.prefix {
		font-size: var(--md-sys-typescale-label-sm);
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.name {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
</style>
