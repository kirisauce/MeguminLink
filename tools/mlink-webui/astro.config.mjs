// @ts-check
import { defineConfig } from 'astro/config';
import svelte from '@astrojs/svelte';

console.log(process.env.ASTRO_BASE)

// https://astro.build/config
export default defineConfig({
	base: process.env.ASTRO_BASE || '/',
	integrations: [svelte()],
});
