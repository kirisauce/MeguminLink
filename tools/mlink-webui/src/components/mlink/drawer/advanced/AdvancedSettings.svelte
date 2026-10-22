<script lang="ts">
	import type { MlinkSession } from '../../../../lib/session.svelte';
	import Disclosure from '../../../ui/Disclosure.svelte';
	import LinkSettings from './LinkSettings.svelte';
	import LossSettings from './LossSettings.svelte';
	import RateSettings from './RateSettings.svelte';
	import UartSettings from './UartSettings.svelte';

	let { session }: { session: MlinkSession } = $props();
</script>

<Disclosure summary="高级" variant="section">
	<RateSettings {session} />

	<UartSettings {session} />

	<LossSettings {session} />

	<LinkSettings {session} />
</Disclosure>
