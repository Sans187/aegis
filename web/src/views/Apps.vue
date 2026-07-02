<template>
  <h2 class="page-title">软件管理</h2>

  <el-alert v-if="!secSet" type="warning" :closable="false" style="margin-bottom:14px"
    title="你还没有设置安全密码。添加/删除软件、删除卡种需要安全密码，请先到「安全设置」设置。" />

  <div class="toolbar">
    <el-button type="success" :icon="Plus" :disabled="!secSet" @click="openAdd">添加软件</el-button>
    <el-button :icon="Refresh" @click="load">刷新</el-button>
  </div>

  <div class="card">
    <el-table :data="apps" v-loading="loading" stripe>
      <el-table-column label="软件ID (app_id)" width="150">
        <template #default="{ row }"><span class="mono" style="font-weight:600">{{ row.id }}</span></template>
      </el-table-column>
      <el-table-column prop="name" label="名称" min-width="160" />
      <el-table-column label="模式" width="90">
        <template #default="{ row }"><el-tag size="small">{{ row.mode === 'card' ? '卡密' : '账号' }}</el-tag></template>
      </el-table-column>
      <el-table-column label="创建时间" width="170">
        <template #default="{ row }">{{ fmt(row.created_at) }}</template>
      </el-table-column>
      <el-table-column label="操作" :width="isMobile ? 96 : 320" fixed="right">
        <template #default="{ row }">
          <RowActions :actions="[
            { label: '卡种', type: 'primary', plain: true, on: () => openTypes(row) },
            { label: '设置', type: 'warning', plain: true, on: () => openSettings(row) },
            { label: '改名', on: () => rename(row) },
            { label: '删除', type: 'danger', disabled: !secSet, on: () => del(row) },
          ]" />
        </template>
      </el-table-column>
    </el-table>
  </div>

  <!-- 添加软件 -->
  <el-dialog v-model="addVisible" title="添加软件" width="420px">
    <el-form label-width="90px">
      <el-form-item label="软件名称"><el-input v-model="form.name" placeholder="如：我的游戏辅助" /></el-form-item>
      <el-form-item label="模式">
        <el-radio-group v-model="form.mode">
          <el-radio value="card">卡密模式</el-radio>
          <el-radio value="user">账号模式</el-radio>
        </el-radio-group>
      </el-form-item>
      <el-form-item label="安全密码"><el-input v-model="form.security_password" type="password" show-password placeholder="二级密码" /></el-form-item>
      <el-alert type="info" :closable="false" title="app_id 将自动分配（如 100006），创建后请用它配置客户端。" />
    </el-form>
    <template #footer>
      <el-button @click="addVisible = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="doAdd">创建</el-button>
    </template>
  </el-dialog>

  <!-- 卡种管理 -->
  <el-dialog v-model="typesVisible" :title="`卡种管理 · ${typesApp?.name} (${typesApp?.id})`" width="640px">
    <div class="toolbar">
      <el-input v-model="ct.name" placeholder="卡种名称，如 月卡" style="width:140px" />
      <el-input v-model="ct.prefix" placeholder="前缀 如 VIP" maxlength="4" style="width:120px" />
      <el-input-number v-model="ct.hours" :min="1" placeholder="时长" /> <span style="color:#8a90a2">小时</span>
      <el-button type="success" :icon="Plus" @click="addType">添加卡种</el-button>
    </div>
    <el-table :data="types" v-loading="typesLoading" stripe size="small">
      <el-table-column prop="name" label="名称" />
      <el-table-column prop="prefix" label="前缀"><template #default="{ row }"><span class="mono">{{ row.prefix }}</span></template></el-table-column>
      <el-table-column label="时长"><template #default="{ row }">{{ row.hours }} 小时</template></el-table-column>
      <el-table-column label="操作" width="90">
        <template #default="{ row }">
          <el-button size="small" type="danger" @click="delType(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>
    <el-alert type="warning" :closable="false" style="margin-top:10px"
      title="删除卡种会连带删除该卡种下的所有卡密，且不可恢复，需输入安全密码确认。" />
  </el-dialog>

  <!-- 软件设置 -->
  <el-dialog v-model="settingsVisible" :title="`软件设置 · ${settingsApp?.name} (${settingsApp?.id})`" width="640px">
    <div v-loading="settingsLoading">
      <template v-if="s">
        <el-divider content-position="left">换绑策略</el-divider>
        <el-form label-width="130px">
          <el-form-item label="允许换绑"><el-switch v-model="s.rebind.allow" /></el-form-item>
          <el-form-item label="最大换绑次数"><el-input-number v-model="s.rebind.maxTimes" :min="0" /></el-form-item>
          <el-form-item label="换绑冷却(分钟)"><el-input-number v-model="s.rebind.cooldownMinutes" :min="0" /></el-form-item>
          <el-form-item label="超额扣时(小时)"><el-input-number v-model="s.rebind.unbindDeductHours" :min="0" /></el-form-item>
        </el-form>

        <el-divider content-position="left">公告</el-divider>
        <el-form label-width="130px">
          <el-form-item label="启用公告"><el-switch v-model="s.notice.enabled" /></el-form-item>
          <el-form-item label="公告内容"><el-input v-model="s.notice.text" type="textarea" :rows="3" /></el-form-item>
        </el-form>

        <el-divider content-position="left">版本更新</el-divider>
        <el-form label-width="130px">
          <el-form-item label="最新版本"><el-input v-model="s.update.latestVersion" /></el-form-item>
          <el-form-item label="强制更新"><el-switch v-model="s.update.forceUpdate" /></el-form-item>
          <el-form-item label="最低可用版本"><el-input v-model="s.update.minVersion" /></el-form-item>
          <el-form-item label="下载地址"><el-input v-model="s.update.downloadUrl" /></el-form-item>
          <el-form-item label="更新日志"><el-input v-model="s.changelog.text" type="textarea" :rows="3" /></el-form-item>
        </el-form>

        <el-divider content-position="left">验证参数</el-divider>
        <el-form label-width="130px">
          <el-form-item label="Token 有效(小时)"><el-input-number v-model="s.verify.tokenTtlHours" :min="1" /></el-form-item>
        </el-form>
      </template>
    </div>
    <template #footer>
      <el-button @click="settingsVisible = false">取消</el-button>
      <el-button type="primary" :loading="settingsSaving" @click="saveSettings">保存设置</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { Plus, Refresh } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import api from '../api'
import { fmt } from '../utils/time'
import { useAppStore } from '../stores/app'
import { useMobile } from '../composables/useMobile'
import RowActions from '../components/RowActions.vue'

const appStore = useAppStore()
const { isMobile } = useMobile()
const apps = ref([])
const loading = ref(false)
const secSet = ref(false)

async function load() {
  loading.value = true
  try {
    apps.value = (await api.get('/api/apps')).items
    secSet.value = (await api.get('/api/security/status')).set
  } finally { loading.value = false }
}

// ---- 添加软件 ----
const addVisible = ref(false)
const saving = ref(false)
const form = ref({ name: '', mode: 'card', security_password: '' })
function openAdd() { form.value = { name: '', mode: 'card', security_password: '' }; addVisible.value = true }
async function doAdd() {
  if (!form.value.name) return ElMessage.warning('请填写名称')
  if (!form.value.security_password) return ElMessage.warning('请输入安全密码')
  saving.value = true
  try {
    const r = await api.post('/api/apps/create', form.value)
    ElMessage.success(`已创建，app_id = ${r.id}`)
    addVisible.value = false
    await appStore.loadApps(); load()
  } finally { saving.value = false }
}

async function rename(row) {
  const { value } = await ElMessageBox.prompt('新的软件名称', '改名', { inputValue: row.name })
  await api.post('/api/apps/rename', { app_id: row.id, name: value })
  ElMessage.success('已改名'); await appStore.loadApps(); load()
}

async function del(row) {
  const { value: secpw } = await ElMessageBox.prompt(
    `确定删除软件「${row.name}」(${row.id})？这会清空它的全部卡密和用户，且不可恢复！请输入安全密码确认：`,
    '危险操作', { inputType: 'password', confirmButtonText: '删除', confirmButtonClass: 'el-button--danger' })
  await api.post('/api/apps/delete', { app_id: row.id, security_password: secpw })
  ElMessage.success('已删除'); await appStore.loadApps(); load()
}

// ---- 卡种管理 ----
const typesVisible = ref(false)
const typesApp = ref(null)
const types = ref([])
const typesLoading = ref(false)
const ct = ref({ name: '', prefix: '', hours: 720 })

async function openTypes(row) {
  typesApp.value = row
  typesVisible.value = true
  ct.value = { name: '', prefix: '', hours: 720 }
  loadTypes()
}
async function loadTypes() {
  typesLoading.value = true
  try { types.value = (await api.get('/api/card-types', { params: { app_id: typesApp.value.id } })).items }
  finally { typesLoading.value = false }
}
async function addType() {
  if (!ct.value.name || !ct.value.prefix) return ElMessage.warning('请填写名称和前缀')
  await api.post('/api/card-types', { app_id: typesApp.value.id, ...ct.value })
  ElMessage.success('已添加卡种'); ct.value = { name: '', prefix: '', hours: 720 }; loadTypes()
}
async function delType(row) {
  const { value: secpw } = await ElMessageBox.prompt(
    `确定删除卡种「${row.name}」？该卡种下的所有卡密都会被一并删除，不可恢复！请输入安全密码确认：`,
    '危险操作', { inputType: 'password', confirmButtonText: '删除', confirmButtonClass: 'el-button--danger' })
  const r = await api.post('/api/card-types/delete', { app_id: typesApp.value.id, id: row.id, security_password: secpw })
  ElMessage.success(`已删除卡种，连带删除 ${r.removed_cards} 张卡密`); loadTypes()
}

// ---- 软件设置 ----
const settingsVisible = ref(false)
const settingsApp = ref(null)
const s = ref(null)
const settingsLoading = ref(false)
const settingsSaving = ref(false)

async function openSettings(row) {
  settingsApp.value = row
  s.value = null
  settingsVisible.value = true
  settingsLoading.value = true
  try {
    const r = await api.get('/api/apps/settings', { params: { app_id: row.id } })
    s.value = r.settings
  } finally { settingsLoading.value = false }
}
async function saveSettings() {
  settingsSaving.value = true
  try {
    await api.post('/api/apps/settings', { app_id: settingsApp.value.id, settings: s.value })
    ElMessage.success('已保存')
    settingsVisible.value = false
  } finally { settingsSaving.value = false }
}

onMounted(load)
</script>
