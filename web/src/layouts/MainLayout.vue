<template>
  <div class="layout">
    <aside class="sidebar" :class="{ collapsed: collapsed && !isMobile, 'sidebar-mobile': isMobile, open: mobileOpen }">
      <div class="brand">
        <span class="dot"></span>
        <span v-show="!collapsed">Aegis</span>
      </div>

      <div class="nav-scroll">
        <div class="nav-group" v-show="!collapsed">全局</div>
        <div v-for="m in globalMenus" :key="m.path" class="nav-item"
             :class="{ active: route.path === m.path }" :title="m.label" @click="go(m.path)">
          <el-icon><component :is="m.icon" /></el-icon>
          <span v-show="!collapsed">{{ m.label }}</span>
        </div>

        <div class="nav-group" v-show="!collapsed">软件</div>
        <div v-for="m in appMenus" :key="m.path" class="nav-item"
             :class="{ active: route.path === m.path }" :title="m.label" @click="go(m.path)">
          <el-icon><component :is="m.icon" /></el-icon>
          <span v-show="!collapsed">{{ m.label }}</span>
        </div>
      </div>

      <!-- 左下角：当前软件切换 + 收起 -->
      <div class="sidebar-footer">
        <template v-if="!collapsed">
          <div class="footer-label">当前软件</div>
          <el-select v-model="appStore.currentAppId" size="small" style="width:100%"
                     :disabled="!appStore.apps.length" @change="appStore.setApp">
            <el-option v-for="a in appStore.apps" :key="a.id" :label="`${a.name} (${a.id})`" :value="a.id" />
          </el-select>
        </template>
        <el-dropdown v-else trigger="click" placement="right-end" @command="appStore.setApp">
          <button class="footer-app-btn" :title="`当前软件：${currentAppName}`"><el-icon><Grid /></el-icon></button>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item v-for="a in appStore.apps" :key="a.id" :command="a.id">{{ a.name }} ({{ a.id }})</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>

        <div class="collapse-toggle" @click="toggleCollapse">
          <el-icon><component :is="collapsed ? 'Expand' : 'Fold'" /></el-icon>
          <span v-show="!collapsed">收起侧栏</span>
        </div>
      </div>
    </aside>
    <div v-if="isMobile && mobileOpen" class="sidebar-overlay" @click="mobileOpen = false"></div>

    <div class="main">
      <header class="topbar">
        <el-icon v-if="isMobile" class="hamburger" @click="mobileOpen = true"><Fold /></el-icon>
        <span class="topbar-label" style="color:#8a90a2;font-size:13px">当前软件</span>
        <span style="font-weight:600">{{ currentAppName }}</span>
        <div class="spacer"></div>
        <el-dropdown @command="onCommand">
          <span style="cursor:pointer;display:flex;align-items:center;gap:6px">
            <el-icon><UserFilled /></el-icon>
            {{ auth.agent?.nickname || auth.agent?.username }}
            <el-tag v-if="auth.isSuper" size="small" type="danger">超管</el-tag>
            <el-icon><ArrowDown /></el-icon>
          </span>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item command="password">修改密码</el-dropdown-item>
              <el-dropdown-item command="logout" divided>退出登录</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </header>
      <main class="content"><router-view /></main>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useAuth } from '../stores/auth'
import { useAppStore } from '../stores/app'

const route = useRoute()
const router = useRouter()
const auth = useAuth()
const appStore = useAppStore()

const collapsed = ref(localStorage.getItem('aegis_sidebar_collapsed') === '1')
function toggleCollapse() {
  collapsed.value = !collapsed.value
  localStorage.setItem('aegis_sidebar_collapsed', collapsed.value ? '1' : '0')
}

// 手机端：侧栏改为滑出抽屉
const isMobile = ref(window.innerWidth <= 768)
const mobileOpen = ref(false)
function onResize() {
  isMobile.value = window.innerWidth <= 768
  if (!isMobile.value) mobileOpen.value = false
}
function go(p) { router.push(p); mobileOpen.value = false }

const allGlobalMenus = [
  { path: '/dashboard', label: '数据总览', icon: 'DataLine', show: () => true },
  { path: '/agents', label: '代理管理', icon: 'Avatar', show: () => auth.isSuper || auth.can('allowAddChild') },
  { path: '/apps', label: '软件管理', icon: 'Grid', show: () => auth.isSuper },
  { path: '/security', label: '安全设置', icon: 'Lock', show: () => auth.isSuper },
]
const globalMenus = computed(() => allGlobalMenus.filter((m) => m.show()))
const canBatchUsers = () => auth.isSuper || auth.can('extenduser') || auth.can('freezeOp') || auth.can('unbindOp') || auth.can('delCardAndUser')
const canBatchCards = () => auth.isSuper || auth.can('freezeOp') || auth.can('delCardAndUser')
const allAppMenus = [
  { path: '/users', label: '用户', icon: 'User', show: () => true },
  { path: '/cards', label: '卡密', icon: 'CreditCard', show: () => true },
  { path: '/make', label: '制卡', icon: 'Plus', show: () => auth.isSuper || auth.can('cardmaking') },
  { path: '/batch-users', label: '批处理用户', icon: 'Operation', show: canBatchUsers },
  { path: '/batch-cards', label: '批处理卡密', icon: 'Tickets', show: canBatchCards },
]
const appMenus = computed(() => allAppMenus.filter((m) => m.show()))
const currentAppName = computed(() => appStore.apps.find((a) => a.id === appStore.currentAppId)?.name || '未选择')

onMounted(() => { appStore.loadApps(); window.addEventListener('resize', onResize) })
onUnmounted(() => window.removeEventListener('resize', onResize))

async function onCommand(cmd) {
  if (cmd === 'password') router.push('/password')
  else if (cmd === 'logout') { await auth.logout(); router.push('/login') }
}
</script>
