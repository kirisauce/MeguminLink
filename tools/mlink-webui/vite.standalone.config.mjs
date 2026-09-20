// @ts-check
import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { fileURLToPath } from 'node:url';

// 单文件版本：以 standalone/ 为根打包，JS 与 CSS 全部内联进一张 HTML，产出
// dist/mlink-webui-standalone.html。它不引用任何外部资源，可以直接以 file:// 打开
// 使用，也可以随站点一起提供、由用户按需下载。
//
// 这一步运行在 astro build 之后、写进同一个 dist/，因此 emptyOutDir 必须保持
// 关闭——打开会把常规站点的产物一起清掉。
const entry = fileURLToPath(new URL('./standalone/mlink-webui-standalone.html', import.meta.url));

export default defineConfig({
	root: 'standalone',
	plugins: [svelte(), viteSingleFile()],
	build: {
		outDir: '../dist',
		emptyOutDir: false,
		rollupOptions: {
			input: entry,
		},
	},
});
