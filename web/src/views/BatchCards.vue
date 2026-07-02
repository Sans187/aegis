<template>
  <h2 class="page-title">批处理卡密</h2>

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
        <el-input v-model="q.batch_id" placeholder="精确批次" clearable />
      </div>
      <div class="filter">
        <label>状态</label>
        <el-select v-model="q.status" placeholder="全部" clearable>
          <el-option label="未使用" value="unused" /><el-option label="已使用" value="used" />
        </el-select>
      </div>
      <div class="filter">
        <label>冻结状态</label>
        <el-select v-model="q.frozen" placeholder="全部" clearable>
          <el-option label="正常" value="0" /><el-option label="已冻结" value="1" />
        </el-select>
      </div>
      <div class="filter wide">
        <label>制卡时间</label>
        <el-date-picker v-model="q.createdRange" type="datetimerange" start-placeholder="起" end-placeholder="止" />
      </div>
      <div class="filter wide">
        <label>使用时间</label>
        <el-date-picker v-model="q.usedRange" type="datetimerange" start-placeholder="起" end-placeholder="止" />
      </div>
    </div>
    <div class="filter-actions">
      <el-button type="primary" :icon="Search" @click="search">查询</el-button>
      <el-button :icon="RefreshLeft" @click="reset">重置</el-button>
      <el-button :icon="Document" @click="batchVisible = true">批量搜索</el-button>
      <el-tag v-if="batchCodes" type="warning" closable @close="clearBatch">批量搜索中（{{ batchCount }} 条卡号）</el-tag>
    </div>
  </div>

  <el-dialog v-model="batchVisible" title="批量搜索卡密（每行一个卡号）" width="460px">
    <el-input v-model="batchInput" type="textarea" :rows="12" placeholder="每行粘贴一个卡号（最多 2000 条）" />
    <template #footer>
      <el-button @click="batchVisible = false">取消</el-button>
      <el-button type="primary" @click="doBatch">搜索</el-button>
    </template>
  </el-dialog>

  <!-- 列表 + 批处理 -->
  <div class="card">
    <div class="filter-actions" style="margin-bottom:12px">
      <el-tag :type="effCount ? 'info' : ''">已选 {{ effCount }} 张卡密{{ allActive ? '（全部匹配）' : '' }}</el-tag>
      <el-button v-if="!allActive && total" size="small" type="primary" plain :loading="allLoading" @click="selectAllMatching">选中全部匹配（{{ total }}）</el-button>
      <el-button v-if="auth.can('freezeOp')" size="small" :disabled="!effCount" @click="batchFreeze(1)">批量冻结</el-button>
      <el-button v-if="auth.can('freezeOp')" size="small" :disabled="!effCount" @click="batchFreeze(0)">批量解冻</el-button>
      <el-button size="small" type="danger" :disabled="!effCount" @click="batchDelete">批量删除</el-button>
      <el-button v-if="effCount" size="small" text @click="clearSel">取消选择</el-button>
    </div>
    <el-table ref="tableRef" :data="cards" v-loading="loading" stripe row-key="id" @selection-change="onSelect">
      <el-table-column type="selection" width="46" reserve-selection />
      <el-table-column prop="code" label="卡号" min-width="200">
        <template #default="{ row }"><span class="mono">{{ row.code }}</span></template>
      </el-table-column>
      <el-table-column label="卡种" min-width="130" show-overflow-tooltip><template #default="{ row }">{{ row.card_type || '-' }}</template></el-table-column>
      <el-table-column label="状态" width="140">
        <template #default="{ row }">
          <el-tag :type="row.status === 'unused' ? 'success' : 'info'" size="small">{{ row.status === 'unused' ? '未使用' : '已使用' }}</el-tag>
          <el-tag v-if="row.frozen" type="danger" size="small">冻结</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="maker_name" label="制卡人" width="110" />
      <el-table-column label="制卡时间" width="160"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
      <el-table-column label="使用时间" width="160"><template #default="{ row }">{{ row.used_at ? fmt(row.used_at) : '-' }}</template></el-table-column>
    </el-table>
    <el-pagination style="margin-top:14px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="total" :page-size="pageSize" :current-page="page" @current-change="(p) => { page = p; reload() }" />
  </div>
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

const blankQuery = () => ({ owner: '', ownerSub: false, batch_id: '', status: '', frozen: '', createdRange: null, usedRange: null })
const q = reactive(blankQuery())
const cards = ref([])
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
  if (q.status) p.status = q.status
  if (q.frozen !== '') p.frozen = q.frozen
  if (q.owner) { p.owner = q.owner; if (q.ownerSub) p.ownerSub = '1' }
  if (q.createdRange?.length === 2) { p.createdRangeStart = toEpoch(q.createdRange[0]); p.createdRangeEnd = toEpoch(q.createdRange[1]) }
  if (q.usedRange?.length === 2) { p.usedRangeStart = toEpoch(q.usedRange[0]); p.usedRangeEnd = toEpoch(q.usedRange[1]) }
  return p
}

async function reload() {
  if (!appStore.currentAppId) return
  loading.value = true
  try {
    let r
    if (batchCodes.value) {
      r = await api.post('/api/cards/batch', { app_id: appStore.currentAppId, codes: batchCodes.value, page: page.value, pageSize })
      batchCount.value = r.queried || 0
    } else {
      r = await api.get('/api/cards', { params: { ...buildParams(), page: page.value, pageSize } })
    }
    cards.value = r.items; total.value = r.total
  } finally { loading.value = false }
}
function search() { batchCodes.value = ''; page.value = 1; clearSel(); reload() }
function reset() { Object.assign(q, blankQuery()); batchCodes.value = ''; batchInput.value = ''; page.value = 1; clearSel(); reload() }

const batchVisible = ref(false)
const batchInput = ref('')
const batchCodes = ref('')
const batchCount = ref(0)
function doBatch() {
  if (!batchInput.value.trim()) return ElMessage.warning('请粘贴卡号')
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
      ids = (await api.post('/api/cards/batch', { app_id: appStore.currentAppId, codes: batchCodes.value, idsOnly: true })).ids
    } else {
      ids = (await api.get('/api/cards', { params: { ...buildParams(), idsOnly: 1 } })).ids
    }
    tableRef.value?.clearSelection(); selected.value = []
    allIds.value = ids || []; allActive.value = true
    if (allIds.value.length >= 20000) ElMessage.warning('匹配结果过多，已选中前 20000 条，请进一步缩小筛选范围')
  } finally { allLoading.value = false }
}

async function batchFreeze(frozen) {
  const verb = frozen ? '冻结' : '解冻'
  await ElMessageBox.confirm(`确定${verb}选中的 ${effCount.value} 张卡密？`, `批量${verb}`, { type: 'warning' })
  const r = await api.post('/api/cards/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: frozen ? 'freeze' : 'unfreeze' })
  ElMessage.success(`已${verb} ${r.affected} 张`); clearSel(); reload()
}
async function batchDelete() {
  let delUsers = false
  if (auth.can('delCardAndUser')) {
    // 有权限：可删已使用的卡，并可选连带删用户
    try {
      await ElMessageBox.confirm(
        `删除选中的 ${effCount.value} 张卡密时，是否同时删除使用过这些卡的用户？此操作不可恢复！`,
        '批量删除卡密',
        { confirmButtonText: '连用户一起删', cancelButtonText: '只删卡密', distinguishCancelAndClose: true, type: 'warning' })
      delUsers = true
    } catch (a) { if (a === 'close') return; delUsers = false }
  } else {
    // 无权限：只能删未使用的卡密
    await ElMessageBox.confirm(
      `你没有「删除卡/用户」权限，将只删除选中卡密里“未使用”的部分（已使用的会跳过）。确定？`,
      '批量删除卡密', { type: 'warning' })
  }
  const r = await api.post('/api/cards/batch-op', { app_id: appStore.currentAppId, ids: effIds(), action: 'delete', delete_used_users: delUsers })
  ElMessage.success(`已删除 ${r.affected} 张卡密` + (delUsers ? `，连带删除 ${r.affected_users} 个用户` : ''))
  clearSel(); reload()
}

watch(() => appStore.currentAppId, () => { page.value = 1; clearSel(); reload() })
onMounted(() => { loadSubAgents(); reload() })
</script>
