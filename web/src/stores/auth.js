import { defineStore } from 'pinia'
import api from '../api'

export const useAuth = defineStore('auth', {
  state: () => ({
    token: localStorage.getItem('aegis_token') || '',
    agent: null,
    mustChange: false,
  }),
  getters: {
    isLoggedIn: (s) => !!s.token,
    isSuper: (s) => s.agent?.level === 1,
    perms: (s) => s.agent?.perms || [],
  },
  actions: {
    can(p) {
      return this.isSuper || (this.agent?.perms || []).includes(p)
    },
    setToken(t) {
      this.token = t
      localStorage.setItem('aegis_token', t)
    },
    async login(username, password) {
      const r = await api.post('/api/auth/login', { username, password })
      this.setToken(r.token)
      this.agent = r.agent
      this.mustChange = !!r.must_change_password
      return r
    },
    async fetchMe() {
      const r = await api.get('/api/auth/me')
      this.agent = r.agent
      this.mustChange = !!r.agent.must_change_password
      return r
    },
    async logout() {
      try { await api.post('/api/auth/logout') } catch (e) { /* ignore */ }
      this.token = ''
      this.agent = null
      localStorage.removeItem('aegis_token')
    },
  },
})
