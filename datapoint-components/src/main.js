import { createApp } from 'vue'
import App from './App.vue'
import router from './router'
import { dserv } from './services/minimal-dserv.js' // Updated path

const app = createApp(App)
app.config.globalProperties.$dserv = dserv
app.use(router)
app.mount('#app')