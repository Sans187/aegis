<template>
  <h2 class="page-title">用户管理</h2>

  <!-- 搜索面板 -->
  <div class="card" style="margin-bottom:16px">
    <div class="filters">
      <div class="filter">
        <label>搜索字段</label>
        <el-select v-model="q.field">
          <el-option label="全部字段" value="all" />
          <el-option label="用户名" value="username" />
          <el-option label="机器码" value="machine_code" />
          <el-option label="IP" value="ip_address" />
          <el-option label="备注" value="remark" />
          <el-option label="附属项" value="extra" />
          <el-option label="卡密" value="code" />
        </el-select>
      </div>
      <div class="filter">
        <label>关键词</label>
        <el-input v-model="q.keyword" placeholder="模糊搜索" clearable @keyup.enter="search" />
      </div>
      <div class="filter">
        <label>卡种</label>
        <el-select v-model="q.type_id" placeholder="全部" clearable>
          <el-option v-for="t in types" :key="t.id" :label="t.name" :value="t.id" />
        </el-select>
      </div>
      <div class="filter">
        <label>卡密所属人</label>
        <el-select v-model="q.owner" placeholder="全部" clearable filterable>
          <el-option v-for="a in subAgents" :key="a.id"
                     :label="(a.nickname ? a.nickname + ' / ' : '') + a.username + (a.self ? '（我）' : '')" :value="a.id" />
        </el-select>
      </div>
      <div class="filter">
        <label>所属范围</label>
        <el-checkbox v-model="q.ownerSub" :disabled="!q.owner" border>含下级</el-checkbox>
      </div>
      <div class="filter">
        <label>冻结状态</label>
        <el-select v-model="q.frozen" placeholder="全部" clearable>
          <el-option label="正常" value="0" /><el-option label="已冻结" value="1" />
        </el-select>
      </div>
      <div class="filter">
        <label>在线状态</label>
        <el-select v-model="q.online" placeholder="全部" clearable>
          <el-option label="在线" value="1" /><el-option label="离线" value="0" />
        </el-select>
      </div>
      <div class="filter">
        <label>到期状态</label>
        <el-select v-model="q.expireStatus" placeholder="全部" clearable>
          <el-option label="有效" value="valid" /><el-option label="已过期" value="expired" />
        </el-select>
      </div>
      <div class="filter wide">
        <label>创建时间</label>
        <el-date-picker v-model="q.createdRange" type="datetimerange" start-placeholder="起" end-placeholder="止" />
      </div>
      <div class="filter wide">
        <label>到期时间</label>
        <el-date-picker v-model="q.expireRange" type="datetimerange" start-placeholder="起" end-placeholder="止" />
      </div>
    </div>
    <div class="filter-actions">
      <el-button type="primary" :icon="Search" @click="search">查询</el-button>
      <el-button :icon="RefreshLeft" @click="reset">重置</el-button>
      <el-button :icon="Document" @click="batchVisible = true">批量搜索</el-button>
      <el-tag v-if="batchCodes" type="warning" closable @close="clearBatch">批量搜索中（{{ batchCount }} 条卡密）</el-tag>
      <div class="spacer"></div>
      <el-button v-if="isUserMode && auth.can('cardmaking')" type="success" :icon="Plus" @click="createVisible = true">新建账号</el-button>
    </div>
  </div>

  <el-dialog v-model="batchVisible" title="批量搜索用户（按卡密，每行一个）" width="460px">
    <el-input v-model="batchInput" type="textarea" :rows="12" placeholder="每行粘贴一个卡密，查出用过这些卡的用户（最多 2000 条）" />
    <template #footer>
      <el-button @click="batchVisible = false">取消</el-button>
      <el-button type="primary" @click="doBatch">搜索</el-button>
    </template>
  </el-dialog>

  <div class="card">
    <el-table :data="items" v-loading="loading" stripe class="users-table">
      <el-table-column prop="username" label="用户名" min-width="190" show-overflow-tooltip />
      <el-table-column label="创建时间" width="170"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
      <el-table-column label="到期时间" width="170"><template #default="{ row }">{{ fmt(row.expired_at) }}</template></el-table-column>
      <el-table-column prop="remark" label="备注" min-width="110" show-overflow-tooltip />
      <el-table-column label="卡种" min-width="130" show-overflow-tooltip><template #default="{ row }">{{ row.card_type || '-' }}</template></el-table-column>
      <el-table-column prop="extra" label="附属项" min-width="110" show-overflow-tooltip />
      <el-table-column label="状态" width="140">
        <template #default="{ row }">
          <el-tag :type="row.frozen ? 'danger' : 'success'" size="small">{{ row.frozen ? '冻结' : '正常' }}</el-tag>
          <el-tag :type="row.online ? '' : 'info'" size="small" style="margin-left:6px">{{ row.online ? '在线' : '离线' }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" :width="isMobile ? 96 : 300" fixed="right">
        <template #default="{ row }">
          <RowActions :actions="[
            { label: '详情', type: 'primary', plain: true, on: () => openDetail(row) },
            { label: row.frozen ? '解冻' : '冻结', show: auth.can('freezeOp'), on: () => toggleFreeze(row) },
            { label: '续期', show: auth.can('extenduser'), on: () => openExtend(row) },
            { label: '解绑', show: auth.can('unbindOp'), on: () => doUnbind(row) },
            { label: '删除', type: 'danger', show: auth.can('delCardAndUser'), on: () => doDelete(row) },
          ]" />
        </template>
      </el-table-column>
    </el-table>
    <el-pagination style="margin-top:14px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="total" :page-size="pageSize" :current-page="page" @current-change="(p) => { page = p; reload() }" />
  </div>

  <!-- 用户详情 -->
  <el-dialog v-model="detailVisible" title="用户详情" width="760px">
    <template v-if="detail">
      <el-descriptions :column="2" border size="small" label-width="96"
                       :label-style="{ whiteSpace: 'nowrap', width: '96px' }"
                       :content-style="{ minWidth: '200px' }">
        <el-descriptions-item label="用户名">{{ detail.username }}</el-descriptions-item>
        <el-descriptions-item label="状态">
          <el-tag :type="detail.frozen ? 'danger' : 'success'" size="small">{{ detail.frozen ? '冻结' : '正常' }}</el-tag>
          <el-tag :type="detail.online ? '' : 'info'" size="small" style="margin-left:4px">{{ detail.online ? '在线' : '离线' }}</el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="创建时间">{{ fmt(detail.created_at) }}</el-descriptions-item>
        <el-descriptions-item label="到期时间">{{ fmt(detail.expired_at) }}</el-descriptions-item>
        <el-descriptions-item label="机器码" :span="2"><span class="mono">{{ detail.machine_code || '-' }}</span></el-descriptions-item>
        <el-descriptions-item label="换绑次数">{{ detail.rebind_cnt }}</el-descriptions-item>
        <el-descriptions-item label="最近IP">{{ detail.ip_address || '-' }}</el-descriptions-item>
        <el-descriptions-item label="卡种">{{ detail.card_type || '-' }}</el-descriptions-item>
        <el-descriptions-item label="所属代理">{{ detail.agent_name || '-' }}</el-descriptions-item>
        <el-descriptions-item label="备注">{{ detail.remark || '-' }}</el-descriptions-item>
        <el-descriptions-item label="附属项">{{ detail.extra || '-' }}</el-descriptions-item>
      </el-descriptions>
      <div class="page-title" style="font-size:14px;margin:16px 0 8px">充值/激活历史（{{ detail.recharges?.length || 0 }} 次）</div>
      <el-table :data="detail.recharges || []" size="small" stripe>
        <el-table-column label="卡密" min-width="200"><template #default="{ row }"><span class="mono">{{ row.code }}</span></template></el-table-column>
        <el-table-column label="卡种" min-width="120" show-overflow-tooltip><template #default="{ row }">{{ row.type_name || '-' }}</template></el-table-column>
        <el-table-column label="使用时间" width="170"><template #default="{ row }">{{ fmt(row.at) }}</template></el-table-column>
        <el-table-column label="卡密所属人" width="120"><template #default="{ row }">{{ row.maker_name || '-' }}</template></el-table-column>
      </el-table>
      <el-empty v-if="!detail.recharges?.length" description="无充值记录（后台直建账号）" :image-size="60" />
    </template>
  </el-dialog>

  <el-dialog v-model="extendVisible" title="续期" width="320px">
    <el-input-number v-model="extendHours" :min="1" :max="100000" /> 小时
    <template #footer>
      <el-button @click="extendVisible = false">取消</el-button>
      <el-button type="primary" @click="doExtend">确定</el-button>
    </template>
  </el-dialog>

  <el-dialog v-model="createVisible" title="新建账号" width="400px">
    <el-form label-width="80px">
      <el-form-item label="用户名"><el-input v-model="cForm.username" /></el-form-item>
      <el-form-item label="密码"><el-input v-model="cForm.password" type="password" show-password placeholder="该账号的登录密码" /></el-form-item>
      <el-form-item label="时长"><el-input-number v-model="cForm.hours" :min="1" /> 小时</el-form-item>
      <el-form-item label="备注"><el-input v-model="cForm.remark" /></el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="createVisible = false">取消</el-button>
      <el-button type="primary" :loading="creating" @click="doCreate">创建</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed, watch, onMounted } from 'vue'
import { Search, Plus, RefreshLeft, Document } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import api from '../api'
import { fmt, toEpoch } from '../utils/time'
import { useAuth } from '../stores/auth'
import { useAppStore } from '../stores/app'
import { useMobile } from '../composables/useMobile'
import RowActions from '../components/RowActions.vue'

const auth = useAuth()
const appStore = useAppStore()
const { isMobile } = useMobile()

const blankQuery = () => ({ field: 'all', keyword: '', type_id: '', owner: '', ownerSub: false, frozen: '', online: '', expireStatus: '', createdRange: null, expireRange: null })
const q = reactive(blankQuery())
const types = ref([])
const subAgents = ref([])
const items = ref([])
const total = ref(0)
const page = ref(1)
const pageSize = 20
const loading = ref(false)

const currentApp = computed(() => appStore.apps.find((a) => a.id === appStore.currentAppId))
const isUserMode = computed(() => currentApp.value?.mode === 'user')

async function loadTypes() {
  if (!appStore.currentAppId) return
  types.value = (await api.get('/api/card-types', { params: { app_id: appStore.currentAppId } })).items
}
async function loadSubAgents() {
  try { subAgents.value = (await api.get('/api/agents/subtree')).items } catch { subAgents.value = [] }
}

async function reload() {
  if (!appStore.currentAppId) return
  loading.value = true
  try {
    let r
    if (batchCodes.value) {
      r = await api.post('/api/users/batch', { app_id: appStore.currentAppId, codes: batchCodes.value, page: page.value, pageSize })
      batchCount.value = r.queried || 0
    } else {
      const p = { app_id: appStore.currentAppId, page: page.value, pageSize, field: q.field }
      if (q.keyword) p.keyword = q.keyword
      if (q.type_id) p.type_id = q.type_id
      if (q.owner) { p.owner = q.owner; if (q.ownerSub) p.ownerSub = '1' }
      if (q.frozen !== '') p.frozen = q.frozen
      if (q.online !== '') p.online = q.online
      if (q.expireStatus) p.expireStatus = q.expireStatus
      if (q.createdRange?.length === 2) { p.createdRangeStart = toEpoch(q.createdRange[0]); p.createdRangeEnd = toEpoch(q.createdRange[1]) }
      if (q.expireRange?.length === 2) { p.expireRangeStart = toEpoch(q.expireRange[0]); p.expireRangeEnd = toEpoch(q.expireRange[1]) }
      r = await api.get('/api/users', { params: p })
    }
    items.value = r.items; total.value = r.total
  } finally { loading.value = false }
}
function search() { batchCodes.value = ''; page.value = 1; reload() }
function reset() { Object.assign(q, blankQuery()); batchCodes.value = ''; batchInput.value = ''; page.value = 1; reload() }

const batchVisible = ref(false)
const batchInput = ref('')
const batchCodes = ref('')
const batchCount = ref(0)
function doBatch() {
  if (!batchInput.value.trim()) return ElMessage.warning('请粘贴卡密')
  batchCodes.value = batchInput.value
  batchVisible.value = false; page.value = 1; reload()
}
function clearBatch() { batchCodes.value = ''; batchInput.value = ''; page.value = 1; reload() }

async function toggleFreeze(row) {
  const frozen = row.frozen ? 0 : 1
  const verb = frozen ? '冻结' : '解冻'
  let freeze_card = false
  try {
    await ElMessageBox.confirm(`是否同时${verb}该用户使用的卡密？`, `${verb}用户`,
      { confirmButtonText: `是，一起${verb}`, cancelButtonText: `否，仅用户`, distinguishCancelAndClose: true, type: 'warning' })
    freeze_card = true
  } catch (a) { if (a === 'close') return; freeze_card = false }
  await api.post('/api/users/freeze', { app_id: appStore.currentAppId, id: row.id, frozen, freeze_card })
  ElMessage.success('已更新'); reload()
}
async function doUnbind(row) {
  await ElMessageBox.confirm('确定解绑该用户机器码？', '提示', { type: 'warning' })
  await api.post('/api/users/unbind', { app_id: appStore.currentAppId, id: row.id })
  ElMessage.success('已解绑'); reload()
}
async function doDelete(row) {
  await ElMessageBox.confirm(`确定删除用户 ${row.username}？`, '危险操作', { type: 'warning' })
  await api.post('/api/users/delete', { app_id: appStore.currentAppId, id: row.id })
  ElMessage.success('已删除'); reload()
}

const detailVisible = ref(false)
const detail = ref(null)
async function openDetail(row) {
  detail.value = null; detailVisible.value = true
  detail.value = (await api.get('/api/users/detail', { params: { app_id: appStore.currentAppId, id: row.id } })).user
}

const extendVisible = ref(false)
const extendHours = ref(24)
const extendRow = ref(null)
function openExtend(row) { extendRow.value = row; extendHours.value = 24; extendVisible.value = true }
async function doExtend() {
  await api.post('/api/users/extend', { app_id: appStore.currentAppId, id: extendRow.value.id, hours: extendHours.value })
  extendVisible.value = false; ElMessage.success('已续期'); reload()
}

const createVisible = ref(false)
const creating = ref(false)
const cForm = ref({ username: '', password: '', hours: 720, remark: '' })
async function doCreate() {
  if (!cForm.value.username || !cForm.value.password) return ElMessage.warning('用户名和密码必填')
  creating.value = true
  try {
    await api.post('/api/users/create', { app_id: appStore.currentAppId, ...cForm.value })
    ElMessage.success('账号已创建'); createVisible.value = false
    cForm.value = { username: '', password: '', hours: 720, remark: '' }; reload()
  } finally { creating.value = false }
}

watch(() => appStore.currentAppId, () => { page.value = 1; loadTypes(); reload() })
onMounted(() => { loadTypes(); loadSubAgents(); reload() })
</script>
