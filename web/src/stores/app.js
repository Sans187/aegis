import { defineStore } from 'pinia'
import api from '../api'

// 当前选中的应用（大部分页面的操作都以此 app_id 为上下文）。
export const useAppStore = defineStore('app', {
  state: () => ({
    apps: [],
    currentAppId: localStorage.getItem('aegis_app') || '',
  }),
  actions: {
    async loadApps() {
      const r = await api.get('/api/apps')
      this.apps = r.items || []
      if (!this.currentAppId && this.apps.length) this.setApp(this.apps[0].id)
      else if (this.currentAppId && !this.apps.find((a) => a.id === this.currentAppId) && this.apps.length)
        this.setApp(this.apps[0].id)
      return this.apps
    },
    setApp(id) {
      this.currentAppId = id
      localStorage.setItem('aegis_app', id)
    },
  },
})
