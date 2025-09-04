import { createRouter, createWebHistory } from 'vue-router'
import MeshDashboard from '../views/MeshDashboard.vue'

const routes = [
  {
    path: '/',
    name: 'Home',
    redirect: '/mesh'
  },
  {
    path: '/mesh',
    name: 'MeshDashboard',
    component: MeshDashboard,
    meta: {
      title: 'Mesh Network Dashboard'
    }
  }
]

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes
})

// Optional: Update page title based on route
router.beforeEach((to, from, next) => {
  if (to.meta.title) {
    document.title = to.meta.title
  }
  next()
})

export default router