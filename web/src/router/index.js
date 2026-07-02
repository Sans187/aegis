import { createRouter, createWebHashHistory } from 'vue-router'
import { useAuth } from '../stores/auth'

const routes = [
  { path: '/login', component: () => import('../views/Login.vue'), meta: { public: true } },
  {
    path: '/',
    component: () => import('../layouts/MainLayout.vue'),
    redirect: '/dashboard',
    children: [
      // 全局区：总览所有代理可见；代理管理需"添加下级"权限；软件管理/安全设置仅超管
      { path: 'dashboard', component: () => import('../views/Dashboard.vue') },
      { path: 'agents', component: () => import('../views/Agents.vue'), meta: { perm: 'allowAddChild' } },
      { path: 'apps', component: () => import('../views/Apps.vue'), meta: { super: true } },
      { path: 'security', component: () => import('../views/Security.vue'), meta: { super: true } },
      // 软件区
      { path: 'users', component: () => import('../views/Users.vue') },
      { path: 'cards', component: () => import('../views/Cards.vue') },
      { path: 'make', component: () => import('../views/MakeCards.vue'), meta: { perm: 'cardmaking' } },
      { path: 'batch-users', component: () => import('../views/BatchUsers.vue') },
      { path: 'batch-cards', component: () => import('../views/BatchCards.vue') },
      { path: 'password', component: () => import('../views/ChangePassword.vue') },
    ],
  },
]

const router = createRouter({ history: createWebHashHistory(), routes })

router.beforeEach(async (to) => {
  const auth = useAuth()
  if (to.meta.public) return true
  if (!auth.isLoggedIn) return '/login'
  if (!auth.agent) {
    try { await auth.fetchMe() } catch { return '/login' }
  }
  if (auth.mustChange && to.path !== '/password') return '/password'
  // 仅超管页
  if (to.meta.super && !auth.isSuper) return '/dashboard'
  // 需特定权限的页（超管恒通过）
  if (to.meta.perm && !auth.isSuper && !auth.can(to.meta.perm)) return '/dashboard'
  return true
})

export default router
