<template>
  <h2 class="page-title">批处理用户</h2>

  <!-- 搜索面板 -->
  <div class="card" style="margin-bottom:16px">
    <div class="filters">
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
        <label>批次ID</label>
        <el-input v-model="q.batch_id" placeholder="用过该批次卡的用户" clearable />
      </div>
      <div class="filter">
        <label>冻结状态</label>
        <el-select v-model="q.frozen" placeholder="全部" clearable>
          <el-option label="正常" value="0" /><el-option label="已冻结" value="1" />
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
    </div>
  </div>

  <el-dialog v-model="batchVisible" title="批量搜索用户（按卡密，每行一个）" width="460px">
    <el-input v-model="batchInput" type="textarea" :rows="12" placeholder="每行粘贴一个卡密，查出用过这些卡的用户（最多 2000 条）" />
    <template #footer>
      <el-button @click="batchVisible = false">取消</el-button>
      <el-button type="primary" @click="doBatch">搜索</el-button>
    </template>
  </el-dialog>

  <!-- 列表 + 批处理 -->
  <div class="card">
    <div class="filter-actions" style="margin-bottom:12px">
      <el-tag :type="effCount ? 'info' : ''">已选 {{ effCount }} 个用户{{ allActive ? '（全部匹配）' : '' }}</el-tag>
      <el-button v-if="!allActive && total" size="small" type="primary" plain :loading="allLoading" @click="selectAllMatching">选中全部匹配（{{ total }}）</el-button>
      <el-button v-if="auth.can('extenduser')" size="small" :disabled="!effCount" @click="batchExtendVisible = true">批量加时</el-button>
      <el-button v-if="auth.can('freezeOp')" size="small" :disabled="!effCount" @click="batchFreeze(1)">批量冻结</el-button>
      <el-button v-if="auth.can('freezeOp')" size="small" :disabled="!effCount" @click="batchFreeze(0)">批量解冻</el-button>
      <el-button v-if="auth.can('unbindOp')" size="small" :disabled="!effCount" @click="batchUnbind">批量解绑</el-button>
      <el-button v-if="auth.can('delCardAndUser')" size="small" type="danger" :disabled="!effCount" @click="batchDelete">批量删除</el-button>
      <el-button v-if="effCount" size="small" text @click="clearSel">取消选择</el-button>
    </div>
    <el-table ref="tableRef" :data="items" v-loading="loading" stripe row-key="id" @selection-change="onSelect">
      <el-table-column type="selection" width="46" reserve-selection />
      <el-table-column prop="username" label="用户名" min-width="130" />
      <el-table-column label="创建时间" width="160"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
      <el-table-column label="到期时间" width="160"><template #default="{ row }">{{ fmt(row.expired_at) }}</template></el-table-column>
      <el-table-column label="卡种" min-width="130" show-overflow-tooltip><template #default="{ row }">{{ row.card_type || '-' }}</template></el-table-column>
      <el-table-column prop="remark" label="备注" min-width="100" show-overflow-tooltip />
      <el-table-column label="状态" width="140">
        <template #default="{ row }">
          <el-tag :type="row.frozen ? 'danger' : 'success'" size="small">{{ row.frozen ? '冻结' : '正常' }}</el-tag>
          <el-tag :type="row.online ? '' : 'info'" size="small" style="margin-left:6px">{{ row.online ? '在线' : '离线' }}</el-tag>
        </template>
      </el-table-column>
    </el-table>
    <el-pagination style="margin-top:14px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="total" :page-size="pageSize" :current-page="page" @current-change="(p) => { page = p; reload() }" />
  </div>

  <el-dialog v-model="batchExtendVisible" :title="`批量加时（${selected.length} 个用户）`" width="340px">
    <el-input-number v-model="batchExtendHours" :min="1" :max="100000" /> 小时
    <template #footer>
      <el-button @click="batchExtendVisible = false">取消</el-button>
      <el-button type="primary" @click="doBatchExtend">确定</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed, watch, onMounted } from 'vue'
import { Search, RefreshLeft, Document } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import api from '../api'
import { fmt, toEpoch } from '../utils/time'
import { useAuth } from '../stores/auth'
import { useAppStore } from '../stores/app'

const auth = useAuth()
const appStore = useAppStore()

const blankQuery = () => ({ owner: '', ownerSub: false, batch_id: '', frozen: '', expireStatus: '', createdRange: null, expireRange: null })
const q = reactive(blankQuery())
const items = ref([])
const total = ref(0)
const page = ref(1)
const pageSize = 20
const loading = ref(false)
const subAgents = ref([])

async function loadSubAgents() {
  try { subAgents.value = (await api.get('/api/agents/subtree')).items } catch { subAgents.value = [] }
}

// 构造常规筛选参数（供列表与「全选所有匹配」复用）
function buildParams() {
  const p = { app_id: appStore.currentAppId, field: 'all' }
  if (q.batch_id) p.batch_id = q.batch_id
  if (q.frozen !== '') p.frozen = q.frozen
  if (q.expireStatus) p.expireStatus = q.expireStatus
  if (q.owner) { p.owner = q.owner; if (q.ownerSub) p.ownerSub = '1' }
  if (q.createdRange?.length === 2) { p.createdRangeStart = toEpoch(q.createdRange[0]); p.createdRangeEnd = toEpoch(q.createdRange[1]) }
  if (q.expireRange?.length === 2) { p.expireRangeStart = toEpoch(q.expireRange[0]); p.expireRangeEnd = toEpoch(q.expireRange[1]) }
  return p
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
      r = await api.get('/api/users', { params: { ...buildParams(), page: page.value, pageSize } })
    }
    items.value = r.items; total.value = r.total
  } finally { loading.value = false }
}
function search() { batchCodes.value = ''; page.value = 1; clearSel(); reload() }
function reset() { Object.assign(q, blankQuery()); batchCodes.value = ''; batchInput.value = ''; page.value = 1; clearSel(); reload() }

const batchVisible = ref(false)
const batchInput = ref('')
const batchCodes = ref('')
const batchCount = ref(0)
function doBatch() {
  if (!batchInput.value.trim()) return ElMessage.warning('请粘贴卡密')
  batchCodes.value = batchInput.value
  batchVisible.value = false; page.value = 1; reload()
}
function clearBatch() { batchCodes.value = ''; batchInput.value = ''; page.value = 1; clearSel(); reload() }

// ---- 批处理 ----
const tableRef = ref(null)
const selected = ref([])
const allActive = ref(false)   // 「全选所有匹配」模式（按当前筛选，全件）
const allIds = ref([])
const allLoading = ref(false)
const effCount = computed(() => allActive.value ? allIds.value.length : selected.value.length)
const effIds = () => allActive.value ? allIds.value : selected.value.map((r) => r.id)

function onSelect(rows) { selected.value = rows; if (rows.length) allActive.value = false }
function clearSel() { tableRef.value?.clearSelection(); selected.value = []; allActive.value = false; allIds.value = [] }

async function selectAllMatching() {
  allLoading.value = true
  try {
    let ids
    if (batchCodes.value) {
      ids = (await api.post('/api/users/batch', { app_id: appStore.currentAppId, codes: batchCodes.value, idsOnly: true })).ids
    } else {
      ids = (await api.get('/api/users', { params: { ...buildParams(), idsOnly: 1 } })).ids
    }
    tableRef.value?.clearSelection(); selected.value = []
    allIds.value = ids || []; allActive.value = true
    if (allIds.value.length >= 20000) ElMessage.warning('匹配结果过多，已选中前 20000 条，请进一步缩小筛选范围')
  } finally { allLoading.value = false }
}

async function batchFreeze(frozen) {
  const verb = frozen ? '冻结' : '解冻'
  await ElMessageBox.confirm(`确定${verb}选中的 ${effCount.value} 个用户？`, `批量${verb}`, { type: 'warning' })
  const r = await api.post('/api/users/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: frozen ? 'freeze' : 'unfreeze' })
  ElMessage.success(`已${verb} ${r.affected} 个`); clearSel(); reload()
}
async function batchUnbind() {
  await ElMessageBox.confirm(`确定解绑选中的 ${effCount.value} 个用户的机器码？`, '批量解绑', { type: 'warning' })
  const r = await api.post('/api/users/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: 'unbind' })
  ElMessage.success(`已解绑 ${r.affected} 个`); clearSel(); reload()
}
async function batchDelete() {
  await ElMessageBox.confirm(`确定删除选中的 ${effCount.value} 个用户？此操作不可恢复！`, '危险操作',
    { type: 'warning', confirmButtonText: '删除', confirmButtonClass: 'el-button--danger' })
  const r = await api.post('/api/users/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: 'delete' })
  ElMessage.success(`已删除 ${r.affected} 个`); clearSel(); reload()
}
const batchExtendVisible = ref(false)
const batchExtendHours = ref(24)
async function doBatchExtend() {
  const r = await api.post('/api/users/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: 'extend', hours: batchExtendHours.value })
  batchExtendVisible.value = false
  ElMessage.success(`已为 ${r.affected} 个用户加时`); clearSel(); reload()
}

watch(() => appStore.currentAppId, () => { page.value = 1; clearSel(); reload() })
onMounted(() => { loadSubAgents(); reload() })
</script>
