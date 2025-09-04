import { createApp } from 'vue'
import { createRouter, createWebHistory } from 'vue-router'
import App from './App.vue'
import MeshDashboard from './views/MeshDashboard.vue'

const routes = [
  { path: '/', redirect: '/mesh' },
  { path: '/mesh', component: MeshDashboard }
]

const router = createRouter({
  history: createWebHistory(),
  routes
})

const app = createApp(App)
app.use(router)
app.mount('#app')

// src/App.vue - Root component
