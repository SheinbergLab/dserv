import { createRouter, createWebHistory } from 'vue-router'
import HomeView from '../views/HomeView.vue'
import TerminalView from '../views/TerminalView.vue'
import MinimalGraphicsView from '../views/MinimalGraphicsView.vue' // Add this

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes: [
    { path: '/', component: HomeView },
    { path: '/terminal', component: TerminalView },
    { path: '/minimal-graphics', component: MinimalGraphicsView } // Add this
  ]
})

export default router