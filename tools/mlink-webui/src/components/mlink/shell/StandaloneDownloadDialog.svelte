<script lang="ts">
	import { STANDALONE_FILENAME } from '../../../lib/standalone';
	import ConfirmDialog from '../../ui/ConfirmDialog.svelte';

	let { open, href, onclose }: { open: boolean; href: string; onclose: () => void } = $props();

	let busy = $state(false);
	let error = $state<string | null>(null);

	/**
	 * 由本页取回单文件并以 blob 交给浏览器保存。
	 *
	 * 走 fetch 而不是让链接直接带 download：这样能在单文件产物不存在（例如站点只
	 * 部署了常规版本）时拿到状态码并明确告知，而不是静默失败。
	 */
	async function save(): Promise<void> {
		busy = true;
		error = null;
		try {
			const response = await fetch(href);
			if (!response.ok) {
				throw new Error(`服务器返回 ${response.status}`);
			}
			const blob = await response.blob();
			const url = URL.createObjectURL(blob);
			const anchor = document.createElement('a');
			anchor.href = url;
			anchor.download = STANDALONE_FILENAME;
			anchor.click();
			URL.revokeObjectURL(url);
			onclose();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = false;
		}
	}
</script>

<ConfirmDialog
	{open}
	title="下载单文件版本"
	confirmLabel={busy ? '下载中…' : '确定'}
	cancelLabel="取消"
	onconfirm={() => void save()}
	ondismiss={onclose}
>
	{#snippet children()}
		<p>保存到本地，用浏览器打开即可享用~</p>
		<p>
			按「确定」键开始下载；也可以<a href={href}>跳转到该页面</a>，按下 Ctrl+S 保存。
		</p>
		{#if error !== null}
			<p class="error">下载失败：{error}</p>
		{/if}
	{/snippet}
</ConfirmDialog>

<style>
	p {
		margin: 0;
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface-variant);
	}

	p + p {
		margin-top: var(--md-sys-space-sm);
	}

	a {
		color: var(--md-sys-color-primary);
	}

	.error {
		margin-top: var(--md-sys-space-sm);
		color: var(--md-sys-color-error);
	}
</style>
