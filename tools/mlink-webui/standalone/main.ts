/**
 * 单文件版本入口：把监控面板直接挂到空壳页上。
 *
 * 常规站点（src/pages/index.astro）走 Astro 的 island 机制，组件由运行时的
 * 字符串 URL 动态 import，无法内联成一张自包含的 HTML。这里改成客户端直接
 * mount，Vite 便能把全部代码打成单个 chunk，再由 vite-plugin-singlefile
 * 内联进 dist/mlink-webui-standalone.html —— 那个文件可以下载后以 file:// 直接
 * 打开（file:// 属于安全上下文，WebUSB 可用）。
 */

import '../src/styles/tokens.css';
import { mount } from 'svelte';
import Dashboard from '../src/components/mlink/shell/Dashboard.svelte';

const target = document.getElementById('app');
if (target === null) {
	throw new Error('缺少 #app 挂载点');
}

// 离线页自己就是这个产物，不再提供下载入口（file:// 下也解析不到站点根）。
mount(Dashboard, { target, props: { downloadHref: null } });
