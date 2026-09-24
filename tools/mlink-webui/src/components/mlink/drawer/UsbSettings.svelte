<script lang="ts">
	import {
		USB_CAPABILITY_HID_COMPAT,
		USB_CAPABILITY_WEBUSB,
		USB_CAPABILITY_WINUSB,
		USB_DEVICE_NAME_MAX_LEN,
		deviceNameError,
		landingPageError,
	} from '../../../lib/protocol';
	import type { MlinkSession } from '../../../lib/session.svelte';
	import SegmentedControl from '../../ui/SegmentedControl.svelte';

	let { session }: { session: MlinkSession } = $props();

	const config = $derived(session.usbConfig);
	const busy = $derived(session.commandPending || session.device === null);
	/** 编译期未启用的能力置灰：设备端也拒绝把这类项置为启用。 */
	const webusbSupported = $derived(((config?.supported ?? 0) & USB_CAPABILITY_WEBUSB) !== 0);
	const winusbSupported = $derived(((config?.supported ?? 0) & USB_CAPABILITY_WINUSB) !== 0);
	const hidCompatSupported = $derived(((config?.supported ?? 0) & USB_CAPABILITY_HID_COMPAT) !== 0);

	const onOff = [
		{ value: 1, label: '启用' },
		{ value: 0, label: '停用' },
	];

	/** 界面草稿：设备值每次刷新（首次读、保存后重读）都重置，未保存的编辑不跨刷新保留。 */
	let webusb = $state(0);
	let winusb = $state(0);
	let hidCompat = $state(0);
	let landingPage = $state('');
	let deviceName = $state('');

	$effect(() => {
		const current = config;
		if (current === null) return;
		webusb = current.webusb;
		winusb = current.winusb;
		hidCompat = current.hidCompat;
		landingPage = current.landingPage;
		deviceName = current.deviceName;
	});

	const urlError = $derived(landingPageError(landingPage));
	const nameError = $derived(deviceNameError(deviceName));
	const dirty = $derived(
		config !== null &&
			(webusb !== config.webusb ||
				winusb !== config.winusb ||
				hidCompat !== config.hidCompat ||
				landingPage !== config.landingPage ||
				deviceName !== config.deviceName)
	);
	const reconnecting = $derived(session.state === 'connecting' && session.device === null);

	async function save(): Promise<void> {
		await session.saveUsbConfig({ webusb, winusb, hidCompat, landingPage, deviceName });
	}
</script>

<section class="usb-settings">
	<h3>USB 接口</h3>

	<div class="field">
		<span class="label">WebUSB 能力</span>
		<SegmentedControl
			options={onOff}
			value={webusb}
			disabled={busy || !webusbSupported}
			label="WebUSB 能力"
			onchange={(value) => (webusb = value)}
		/>
	</div>

	<div class="field">
		<span class="label">WinUSB 自动识别驱动</span>
		<SegmentedControl
			options={onOff}
			value={winusb}
			disabled={busy || !winusbSupported}
			label="WinUSB 自动识别驱动"
			onchange={(value) => (winusb = value)}
		/>
	</div>

	<div class="field">
		<span class="label">HID 兼容模式</span>
		<SegmentedControl
			options={onOff}
			value={hidCompat}
			disabled={busy || !hidCompatSupported}
			label="HID 兼容模式"
			onchange={(value) => (hidCompat = value)}
		/>
	</div>

	<div class="field">
		<label class="label" for="usb-landing-page">WebUSB landing page</label>
		<input
			id="usb-landing-page"
			type="text"
			bind:value={landingPage}
			disabled={busy || !webusbSupported}
			spellcheck="false"
			autocomplete="off"
			placeholder="https://example.com"
		/>
	</div>

	<div class="field">
		<label class="label" for="usb-device-name">设备名称</label>
		<input
			id="usb-device-name"
			type="text"
			bind:value={deviceName}
			disabled={busy}
			maxlength={USB_DEVICE_NAME_MAX_LEN}
			spellcheck="false"
			autocomplete="off"
			placeholder="留空以使用默认设备名称"
		/>
	</div>

	{#if !webusbSupported && !winusbSupported && !hidCompatSupported}
		<p class="hint">当前固件未编译 WebUSB / WinUSB / HID 兼容模式，没有可调项。</p>
	{/if}

	{#if urlError !== null}
		<p class="error">{urlError}</p>
	{/if}

	{#if nameError !== null}
		<p class="error">{nameError}</p>
	{/if}

	{#if session.commandError !== null}
		<p class="error">{session.commandError}</p>
	{/if}

	{#if reconnecting}
		<p class="hint">设备正在重新枚举 USB，稍后自动接回。</p>
	{/if}

	<button
		class="button"
		type="button"
		disabled={busy || !dirty || urlError !== null || nameError !== null}
		onclick={save}
	>
		保存并重新枚举
	</button>
</section>

<style>
	.usb-settings {
		display: flex;
		flex-direction: column;
		gap: var(--md-sys-space-sm);
	}

	h3 {
		margin: 0;
		font-size: var(--md-sys-typescale-label-sm);
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.06em;
		color: var(--md-sys-color-on-surface-variant);
	}

	.field {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: var(--md-sys-space-md);
	}

	.field:has(input) {
		flex-direction: column;
		align-items: stretch;
		gap: var(--md-sys-space-xs);
	}

	.label {
		font-size: var(--md-sys-typescale-body-md);
		color: var(--md-sys-color-on-surface);
	}

	input {
		font: inherit;
		font-family: var(--md-sys-typescale-font-mono);
		font-size: var(--md-sys-typescale-body-md);
		padding: 0.4rem 0.7rem;
		border: 1px solid var(--md-sys-color-outline-variant);
		border-radius: var(--md-sys-shape-corner-md);
		background: var(--md-sys-color-surface-container);
		color: var(--md-sys-color-on-surface);
		transition:
			border-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter),
			background-color var(--md-sys-motion-duration-short) var(--md-sys-motion-easing-enter);
	}

	input:hover:not(:disabled),
	input:focus-visible {
		border-color: var(--md-sys-color-outline);
	}

	input:disabled {
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

	.hint {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-on-surface-variant);
	}

	.error {
		margin: 0;
		font-size: var(--md-sys-typescale-body-sm);
		color: var(--md-sys-color-error);
	}
</style>
