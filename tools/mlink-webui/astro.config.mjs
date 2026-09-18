// @ts-check
import { defineConfig } from 'astro/config';
import svelte from '@astrojs/svelte';

// https://astro.build/config
export default defineConfig({
	integrations: [svelte()],
	// WebUSB 授权按 origin 区分，端口固定才能让浏览器记住对设备的授权。
	server: { host: '127.0.0.1', port: 4323 },
});
