/**
 * 单文件版本产物：由 vite.standalone.config.mjs 写进 dist/，与站点一起发布。
 * 站点入口、USB 诊断建议与下载弹窗都从这里取名字，避免字面量各处飘移。
 */
export const STANDALONE_FILENAME = 'mlink-webui-standalone.html';

/** 相对于站点根的单文件版本地址。 */
const base = import.meta.env.BASE_URL;
export const STANDALONE_HREF = `${base.endsWith('/') ? base : `${base}/`}${STANDALONE_FILENAME}`;
