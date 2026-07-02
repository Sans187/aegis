import { createApp } from 'vue'
import { createPinia } from 'pinia'
import ElementPlus from 'element-plus'
import 'element-plus/dist/index.css'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import * as Icons from '@element-plus/icons-vue'

import App from './App.vue'
import router from './router'
import api from './api'
import { setTzOffset } from './utils/time'
import './styles.css'

const app = createApp(App)
app.use(createPinia())
app.use(router)
app.use(ElementPlus, { locale: zhCn })
for (const [name, comp] of Object.entries(Icons)) app.component(name, comp)
app.mount('#app')

// 拉取服务器时区偏移，全站时间统一按服务器时区展示/查询
api.get('/api/health').then((r) => setTzOffset(r.tz_offset_min)).catch(() => {})
