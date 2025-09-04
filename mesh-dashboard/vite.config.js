import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { viteSingleFile } from "vite-plugin-singlefile"

export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  server: {
    port: 3000,
    proxy: {
      '/api': {
        target: 'http://localhost:12348',  // Changed from 2565 to 12348
        changeOrigin: true
      },
      '/ws': {
        target: 'ws://localhost:2569',     // WebSocket stays on 2569
        ws: true
      }
    }
  },
  build: {
    outDir: '../www',  // Output directly to www folder
    emptyOutDir: false, // Don't clear other files
    assetsInlineLimit: 100000000 // Inline all assets to ensure single file
  }
})