import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 构建产物用相对路径，方便由 drogon 直接托管（document_root）。
export default defineConfig({
  base: './',
  plugins: [vue()],
  build: {
    outDir: 'dist',
    chunkSizeWarningLimit: 1500,
  },
  server: {
    port: 5173,
    proxy: {
      // 开发期把 /api 转发到后端，避免跨域
      '/api': { target: 'http://127.0.0.1:3000', changeOrigin: true },
    },
  },
})
