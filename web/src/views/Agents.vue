<template>
  <h2 class="page-title">代理管理</h2>

  <div class="toolbar">
    <el-button v-if="canAdd" type="success" :icon="Plus" @click="openCreate">添加下级代理</el-button>
    <el-button v-if="auth.isSuper" :icon="Document" @click="openLogs(null)">登录日志</el-button>
    <span v-if="!auth.isSuper" style="margin-left:auto;color:#666">
      我的余额：<b>{{ money(my.balance) }}</b>　可用：<b style="color:#409eff">{{ money(my.available) }}</b>
    </span>
  </div>

  <div class="card">
    <el-table :data="items" v-loading="loading" stripe>
      <el-table-column prop="username" label="用户名" min-width="120" show-overflow-tooltip />
      <el-table-column prop="nickname" label="昵称" min-width="150" show-overflow-tooltip />
      <el-table-column label="创建人" min-width="120" show-overflow-tooltip>
        <template #default="{ row }">
          <span v-if="row.parent_username">{{ row.parent_nickname || row.parent_username }}<span v-if="row.parent_nickname" class="mono" style="color:#aaa"> / {{ row.parent_username }}</span></span>
          <span v-else style="color:#bbb">—</span>
        </template>
      </el-table-column>
      <el-table-column label="层级" width="64" align="center"><template #default="{ row }">L{{ row.level }}</template></el-table-column>
      <el-table-column label="余额" width="130" align="right"><template #default="{ row }">{{ money(row.balance) }}</template></el-table-column>
      <el-table-column label="可用" width="130" align="right">
        <template #default="{ row }"><span :style="{ color: row.available > 0 ? '#67c23a' : '#999' }">{{ money(row.available) }}</span></template>
      </el-table-column>
      <el-table-column label="可用软件" min-width="150">
        <template #default="{ row }">
          <el-tag v-for="a in row.apps" :key="a" size="small" style="margin:2px">{{ appName(a) }}</el-tag>
          <span v-if="!row.apps?.length" style="color:#bbb">无</span>
        </template>
      </el-table-column>
      <el-table-column label="权限" min-width="200">
        <template #default="{ row }">
          <el-tag v-for="p in row.perms" :key="p" size="small" type="info" style="margin:2px">{{ permLabel(p) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="状态" width="76">
        <template #default="{ row }">
          <el-tag :type="row.status === 'active' ? 'success' : 'info'" size="small">{{ row.status === 'active' ? '正常' : '禁用' }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" :width="isMobile ? 96 : 380" fixed="right">
        <template #default="{ row }">
          <RowActions :actions="[
            { label: '权限', type: 'primary', plain: true, on: () => openEdit(row) },
            { label: '调整余额', type: 'warning', plain: true, show: canAdd, on: () => openBalance(row) },
            { label: '日志', plain: true, show: auth.isSuper, on: () => openLogs(row) },
            { label: row.status === 'active' ? '禁用' : '启用', on: () => toggleStatus(row) },
            { label: '改密', on: () => openReset(row) },
          ]" />
        </template>
      </el-table-column>
    </el-table>
  </div>

  <!-- 新建 / 编辑权限 -->
  <el-dialog v-model="dlg" :title="isEdit ? '编辑代理权限' : '添加下级代理'" width="600px">
    <el-form label-width="92px">
      <template v-if="!isEdit">
        <el-form-item label="用户名"><el-input v-model="form.username" /></el-form-item>
        <el-form-item label="初始密码"><el-input v-model="form.password" type="password" show-password placeholder="至少 8 位" /></el-form-item>
        <el-form-item label="初始余额">
          <el-input-number v-model="form.balance" :min="0" :precision="2" :step="10" />
          <span v-if="!auth.isSuper" style="color:#999;margin-left:10px;font-size:12px">你的可用：{{ money(my.available) }}</span>
        </el-form-item>
      </template>
      <el-form-item label="昵称"><el-input v-model="form.nickname" /></el-form-item>

      <el-form-item label="可用软件">
        <el-select v-model="form.apps" multiple style="width:100%" placeholder="该代理能管理的软件" @change="onAppsChange">
          <el-option v-for="a in grantableApps" :key="a.id" :label="`${a.name} (${a.id})`" :value="a.id" />
        </el-select>
      </el-form-item>

      <el-form-item v-for="a in form.apps" :key="a" :label="`${appName(a)} 卡种`">
        <div style="width:100%">
          <div v-for="t in grantableTypes(a)" :key="t.id" class="ct-row">
            <el-checkbox :model-value="isTypeOn(a, t.id)" @change="(v) => toggleType(a, t.id, v)">
              {{ t.name }}（{{ t.hours }}h）
            </el-checkbox>
            <template v-if="isTypeOn(a, t.id)">
              <span class="ct-price">注册卡价格</span>
              <el-input-number :model-value="form.cardTypes[a][t.id]" :min="typeMin(a, t.id)" :precision="2" :step="1"
                               size="small" controls-position="right" style="width:130px"
                               @change="(v) => setPrice(a, t.id, v)" />
              <span v-if="typeFloor(a, t.id) > 0" class="ct-floor">≥ {{ money(typeFloor(a, t.id)) }}</span>
            </template>
          </div>
          <span v-if="!grantableTypes(a).length" style="color:#bbb">该软件暂无可授权卡种</span>
        </div>
      </el-form-item>

      <el-form-item label="基础权限">
        <el-select v-model="form.perms" multiple style="width:100%" placeholder="授予的权限（不能超过你自己）">
          <el-option v-for="p in grantablePerms" :key="p.key" :label="p.label" :value="p.key" />
        </el-select>
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="dlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="save">{{ isEdit ? '保存' : '创建' }}</el-button>
    </template>
  </el-dialog>

  <!-- 调整余额 -->
  <el-dialog v-model="balVisible" :title="`调整余额 · ${balRow?.username || ''}`" width="400px">
    <div style="margin-bottom:12px;color:#666;line-height:1.9">
      当前余额：<b>{{ money(balRow?.balance) }}</b>　该代理可用：<b>{{ money(balRow?.available) }}</b><br>
      已占用（分配+未用卡+消耗）：{{ money((balRow?.balance || 0) - (balRow?.available || 0)) }}
    </div>
    <el-form label-width="80px">
      <el-form-item label="新余额">
        <el-input-number v-model="balValue" :min="0" :precision="2" :step="10" style="width:200px" />
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="balVisible = false">取消</el-button>
      <el-button type="primary" :loading="balSaving" @click="doAdjustBalance">保存</el-button>
    </template>
  </el-dialog>

  <!-- 改密 -->
  <el-dialog v-model="resetVisible" title="重置密码" width="360px">
    <el-input v-model="resetPw" type="password" show-password placeholder="新密码，至少 8 位" />
    <template #footer>
      <el-button @click="resetVisible = false">取消</el-button>
      <el-button type="primary" @click="doReset">确定</el-button>
    </template>
  </el-dialog>

  <!-- 登录日志（仅超管） -->
  <el-dialog v-model="logsVisible" :title="logsTitle" width="860px">
    <el-table :data="logs" v-loading="logsLoading" stripe size="small" max-height="460">
      <el-table-column v-if="!logsAgent" label="代理" min-width="130">
        <template #default="{ row }">{{ row.nickname ? row.nickname + ' / ' : '' }}{{ row.username }}</template>
      </el-table-column>
      <el-table-column label="登录时间" width="170"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
      <el-table-column label="IP" width="140"><template #default="{ row }"><span class="mono">{{ row.ip || '-' }}</span></template></el-table-column>
      <el-table-column label="设备" min-width="180">
        <template #default="{ row }"><span :title="row.user_agent">{{ uaSummary(row.user_agent) }}</span></template>
      </el-table-column>
      <el-table-column label="状态" width="100">
        <template #default="{ row }">
          <el-tag v-if="row.revoked" type="info" size="small">已注销</el-tag>
          <el-tag v-else-if="row.expires_at * 1000 < Date.now()" type="warning" size="small">已过期</el-tag>
          <el-tag v-else type="success" size="small">有效</el-tag>
        </template>
      </el-table-column>
    </el-table>
    <div v-if="!logs.length && !logsLoading" style="color:#bbb;text-align:center;padding:20px">暂无登录记录</div>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { Plus, Document } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import api from '../api'
import { fmt } from '../utils/time'
import { useAuth } from '../stores/auth'
import { useAppStore } from '../stores/app'
import { useMobile } from '../composables/useMobile'
import RowActions from '../components/RowActions.vue'

const auth = useAuth()
const appStore = useAppStore()
const { isMobile } = useMobile()
const items = ref([])
const loading = ref(false)
const my = ref({ balance: 0, available: 0, super: false })   // 当前代理余额账目

function money(n) { return Number(n || 0).toFixed(2) }

const permOptions = [
  { key: 'cardmaking', label: '制卡' },
  { key: 'batchData', label: '批量数据' },
  { key: 'delCardAndUser', label: '删除卡/用户' },
  { key: 'allowAddChild', label: '添加下级' },
  { key: 'unbindOp', label: '解绑' },
  { key: 'freezeOp', label: '冻结' },
  { key: 'extenduser', label: '用户续期' },
]
function permLabel(k) { return permOptions.find((p) => p.key === k)?.label || k }
function appName(id) { return appStore.apps.find((a) => a.id === id)?.name || id }

const canAdd = computed(() => auth.isSuper || auth.can('allowAddChild'))
const grantableApps = computed(() => appStore.apps)
const grantablePerms = computed(() => auth.isSuper ? permOptions : permOptions.filter((p) => auth.can(p.key)))

// 编辑者对某软件是否"不限卡种"（超管或该软件无授权条目）
function editorAllAllowed(appId) {
  if (auth.isSuper) return true
  const e = auth.agent?.card_types?.[appId]
  if (!e) return true
  return (typeof e === 'object' && !Array.isArray(e) && Object.keys(e).length === 0)
}
const typesByApp = reactive({})
async function fetchTypes(appId) {
  if (typesByApp[appId]) return
  try { typesByApp[appId] = (await api.get('/api/card-types', { params: { app_id: appId } })).items } catch { typesByApp[appId] = [] }
}
// 可授予的卡种 = 该软件全部卡种 ∩ 编辑者自己的额度
function grantableTypes(appId) {
  const all = typesByApp[appId] || []
  if (editorAllAllowed(appId)) return all
  const allowed = auth.agent.card_types[appId]
  const keys = Array.isArray(allowed) ? allowed : Object.keys(allowed || {})
  return all.filter((t) => keys.includes(t.id))
}
// 价格下限 = 编辑者自己对该卡种的价格（超管或不限时为 0）
function typeFloor(appId, tid) {
  if (auth.isSuper) return 0
  const e = auth.agent?.card_types?.[appId]
  if (!e || Array.isArray(e)) return 0
  return Number(e[tid] || 0)
}

// ---- 卡种授权 + 定价的勾选/价格操作 ----
function isTypeOn(appId, tid) {
  // 注意：必须读取 m[tid]（get 会被 Vue 响应式追踪），不能用 hasOwnProperty（不被追踪，会导致复选框点不动）
  const m = form.cardTypes[appId]
  return !!m && m[tid] !== undefined
}
// 价格下限：>0 且不低于父级
function typeMin(appId, tid) { return Math.max(typeFloor(appId, tid), 0.01) }
function toggleType(appId, tid, on) {
  if (!form.cardTypes[appId]) form.cardTypes[appId] = {}
  if (on) form.cardTypes[appId][tid] = typeFloor(appId, tid) || 1   // 默认价（父级价 / 1），不为 0
  else delete form.cardTypes[appId][tid]
}
function setPrice(appId, tid, v) {
  if (!form.cardTypes[appId]) form.cardTypes[appId] = {}
  form.cardTypes[appId][tid] = Math.max(Number(v || 0), typeMin(appId, tid))
}

async function loadMy() {
  try { my.value = await api.get('/api/agents/my-balance') } catch { /* ignore */ }
}
async function load() {
  loading.value = true
  try { items.value = (await api.get('/api/agents')).items } finally { loading.value = false }
}

const dlg = ref(false)
const isEdit = ref(false)
const saving = ref(false)
const form = reactive({ id: '', username: '', password: '', nickname: '', balance: 0, apps: [], perms: [], cardTypes: {} })

function resetForm() {
  Object.assign(form, { id: '', username: '', password: '', nickname: '', balance: 0, apps: [], perms: [], cardTypes: {} })
}
// 兼容老数据：card_types[app] 若是数组，转成 {type:0}
function normalizeCardTypes(src) {
  const out = {}
  for (const a of Object.keys(src || {})) {
    const v = src[a]
    if (Array.isArray(v)) { out[a] = {}; for (const t of v) out[a][t] = 0 }
    else if (v && typeof v === 'object') out[a] = { ...v }
  }
  return out
}
function openCreate() { resetForm(); isEdit.value = false; dlg.value = true }
async function openEdit(row) {
  resetForm()
  isEdit.value = true
  Object.assign(form, {
    id: row.id, nickname: row.nickname || '',
    apps: [...(row.apps || [])], perms: [...(row.perms || [])],
    cardTypes: normalizeCardTypes(row.card_types),
  })
  dlg.value = true
  for (const a of form.apps) await fetchTypes(a)
}
async function onAppsChange() {
  for (const a of form.apps) await fetchTypes(a)
  for (const k of Object.keys(form.cardTypes)) if (!form.apps.includes(k)) delete form.cardTypes[k]
}

async function save() {
  const card_types = {}
  for (const a of form.apps) {
    const m = form.cardTypes[a]
    if (m && Object.keys(m).length) card_types[a] = m
  }
  // 校验：所有已授权卡种的注册卡价格必须 > 0
  for (const a of Object.keys(card_types)) {
    for (const tid of Object.keys(card_types[a])) {
      if (!(Number(card_types[a][tid]) > 0)) return ElMessage.warning('注册卡价格必须大于 0')
    }
  }
  try {
    if (isEdit.value) {
      const r = await api.post('/api/agents/update', { id: form.id, nickname: form.nickname, apps: form.apps, perms: form.perms, card_types })
      if (r?.adjusted_children > 0) ElMessage.success(`已保存，并联动提价 ${r.adjusted_children} 个下级（不低于新价）`)
      else ElMessage.success('已保存')
    } else {
      if (!form.username || form.password.length < 8) return ElMessage.warning('用户名必填，密码至少 8 位')
      saving.value = true
      await api.post('/api/agents', { username: form.username, password: form.password, nickname: form.nickname, balance: form.balance, apps: form.apps, perms: form.perms, card_types })
      ElMessage.success('已创建')
    }
  } finally { saving.value = false }
  dlg.value = false; load(); loadMy()
}

// ---- 调整余额 ----
const balVisible = ref(false)
const balRow = ref(null)
const balValue = ref(0)
const balSaving = ref(false)
function openBalance(row) { balRow.value = row; balValue.value = Number(row.balance || 0); balVisible.value = true }
async function doAdjustBalance() {
  balSaving.value = true
  try {
    await api.post('/api/agents/balance', { id: balRow.value.id, balance: balValue.value })
    ElMessage.success('余额已更新')
    balVisible.value = false; load(); loadMy()
  } finally { balSaving.value = false }
}

async function toggleStatus(row) {
  await api.post('/api/agents/status', { id: row.id, status: row.status === 'active' ? 'disabled' : 'active' })
  ElMessage.success('已更新'); load()
}

const resetVisible = ref(false)
const resetPw = ref('')
const resetRow = ref(null)
function openReset(row) { resetRow.value = row; resetPw.value = ''; resetVisible.value = true }
async function doReset() {
  if (resetPw.value.length < 8) return ElMessage.warning('密码至少 8 位')
  await api.post('/api/agents/reset-password', { id: resetRow.value.id, newPassword: resetPw.value })
  ElMessage.success('已重置'); resetVisible.value = false
}

// ---- 登录日志（仅超管） ----
const logsVisible = ref(false)
const logs = ref([])
const logsLoading = ref(false)
const logsAgent = ref(null)
const logsTitle = computed(() =>
  logsAgent.value
    ? `登录日志 · ${logsAgent.value.nickname ? logsAgent.value.nickname + ' / ' : ''}${logsAgent.value.username}`
    : '登录日志 · 全部代理')

async function openLogs(row) {
  logsAgent.value = row
  logs.value = []
  logsVisible.value = true
  logsLoading.value = true
  try {
    const params = row ? { id: row.id } : {}
    logs.value = (await api.get('/api/agents/login-logs', { params })).items
  } finally { logsLoading.value = false }
}

function uaSummary(ua) {
  if (!ua) return '-'
  let os = '其他'
  if (/Windows NT 10/.test(ua)) os = 'Windows 10/11'
  else if (/Windows/.test(ua)) os = 'Windows'
  else if (/Mac OS X|Macintosh/.test(ua)) os = 'macOS'
  else if (/Android/.test(ua)) os = 'Android'
  else if (/iPhone|iPad|iOS/.test(ua)) os = 'iOS'
  else if (/Linux/.test(ua)) os = 'Linux'
  let br = '其他'
  if (/Edg\//.test(ua)) br = 'Edge'
  else if (/OPR\/|Opera/.test(ua)) br = 'Opera'
  else if (/Firefox\//.test(ua)) br = 'Firefox'
  else if (/Chrome\//.test(ua)) br = 'Chrome'
  else if (/Safari\//.test(ua)) br = 'Safari'
  return `${br} · ${os}`
}

onMounted(() => { appStore.loadApps(); load(); loadMy() })
</script>

<style scoped>
.ct-row { display: flex; align-items: center; gap: 10px; margin: 4px 0; }
.ct-price { color: #888; font-size: 12px; }
.ct-floor { color: #e6a23c; font-size: 12px; }
</style>
