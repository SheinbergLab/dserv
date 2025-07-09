import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import Components from 'unplugin-vue-components/vite'
import { AntDesignVueResolver } from 'unplugin-vue-components/resolvers'

export default defineConfig({
  plugins: [
    vue(),
    Components({
      resolvers: [
        AntDesignVueResolver({
          importStyle: false, // Use CSS-in-JS
        }),
      ],
    }),
  ],
  server: {
    port: 3000,
    // Proxy WebSocket connections to your dserv instance during development
    proxy: {
      '/ws': {
        target: 'ws://localhost:2565',
        ws: true,
        changeOrigin: true
      }
    }
  },
  build: {
    // For later embedding
    rollupOptions: {
      output: {
        manualChunks: undefined,
        inlineDynamicImports: true
      }
    }
  }
})
