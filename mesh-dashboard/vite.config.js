import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { viteSingleFile } from "vite-plugin-singlefile"
// Optionally import basicSsl if you have it installed
// import basicSsl from '@vitejs/plugin-basic-ssl'

export default defineConfig({
  // If you have @vitejs/plugin-basic-ssl installed, uncomment the next line:
  // plugins: [vue(), viteSingleFile(), basicSsl()],
  plugins: [vue(), viteSingleFile()],
  server: {
    port: 3000,
    // Commented out https for now - see below for alternatives
    // https: true,
    proxy: {
      '/api': {
        target: 'https://localhost:12348',  // Backend uses HTTPS
        changeOrigin: true,
        secure: false  // Allow self-signed certificates
      },
      '/ws': {
        target: 'wss://localhost:2569',     // Backend uses WSS
        ws: true,
        secure: false  // Allow self-signed certificates
      }
    }
  },
  build: {
    outDir: '../www',  // Output directly to www folder
    emptyOutDir: false, // Don't clear other files
    assetsInlineLimit: 100000000 // Inline all assets to ensure single file
  }
})
