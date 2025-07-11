import { createApp } from 'vue'
import App from './App.vue'
import router from './router'
import { createPinia } from 'pinia'
import { dserv } from './services/dserv.js'
import { initializeEventTracking } from './services/eventService.js'

// Import Ant Design Vue styles
import 'ant-design-vue/dist/reset.css'

const app = createApp(App)

app.use(createPinia())
app.use(router)

app.mount('#app')

// Wait for dserv to be connected before starting event tracking
const waitForConnection = () => {
  if (dserv.state.connected) {
    initializeEventTracking(dserv)
  } else {
    dserv.on('connection', (data) => {
      if (data.connected) {
        initializeEventTracking(dserv)
      }
    })
  }
}

waitForConnection()
