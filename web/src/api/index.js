import axios from 'axios'
import { ElMessage } from 'element-plus'

// 统一 API 客户端：自动带 Bearer token，401 时跳登录。
const api = axios.create({ baseURL: '/', timeout: 20000 })

api.interceptors.request.use((cfg) => {
  const token = localStorage.getItem('aegis_token')
  if (token) cfg.headers.Authorization = `Bearer ${token}`
  return cfg
})

// 防止并发 401 时重复弹提示
let redirecting = false

api.interceptors.response.use(
  (resp) => resp.data,
  (error) => {
    const status = error?.response?.status
    const msg = error?.response?.data?.error || error.message || '请求失败'
    if (status === 401) {
      localStorage.removeItem('aegis_token')
      // 关键：同时清空 Pinia store（否则 isLoggedIn 仍为 true，跳转不生效，需手动刷新）。
      // 动态 import 避免与 stores/router 形成循环依赖。
      Promise.all([import('../stores/auth'), import('../router')])
        .then(([{ useAuth }, mod]) => {
          try { const a = useAuth(); a.token = ''; a.agent = null } catch { /* pinia 未就绪 */ }
          const router = mod.default
          if (router.currentRoute.value.path === '/login') {
            // 已在登录页：401 = 用户名或密码错误，直接提示（否则页面毫无反馈）。
            ElMessage.error(msg === 'invalid username or password' ? '用户名或密码错误' : msg)
          } else {
            // 其它页面：token 过期，提示后跳回登录页。
            if (!redirecting) { redirecting = true; ElMessage.error('登录已过期，请重新登录') }
            router.replace('/login').finally(() => { redirecting = false })
          }
        })
        .catch(() => { if (!location.hash.startsWith('#/login')) location.hash = '#/login' })
      return Promise.reject(error)
    }
    ElMessage.error(msg)
    return Promise.reject(error)
  },
)

export default api
