<template>
  <h2 class="page-title">卡密管理</h2>

  <div class="card" style="margin-bottom:16px">
    <div class="filters">
      <div class="filter">
        <label>搜索字段</label>
        <el-select v-model="q.field">
          <el-option label="全部字段" value="all" />
          <el-option label="卡号" value="code" />
          <el-option label="备注" value="remark" />
          <el-option label="批次" value="batch_id" />
          <el-option label="制卡人" value="maker_name" />
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
      <div class="filter">
        <label>制卡人</label>
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
      <el-tag v-if="batchCodes" type="warning" closable @close="clearBatch">批量搜索中（{{ batchCount }} 条）</el-tag>
    </div>
  </div>

  <el-dialog v-model="batchVisible" title="批量搜索卡密（每行一个卡号）" width="460px">
    <el-input v-model="batchInput" type="textarea" :rows="12" placeholder="每行粘贴一个卡号（最多 2000 条）" />
    <template #footer>
      <el-button @click="batchVisible = false">取消</el-button>
      <el-button type="primary" @click="doBatch">搜索</el-button>
    </template>
  </el-dialog>

  <div class="card">
    <el-table :data="cards" v-loading="loadingCards" stripe>
      <el-table-column prop="code" label="卡号" min-width="200">
        <template #default="{ row }">
          <span class="mono" style="cursor:pointer" title="点击复制" @click="copyCode(row.code)">{{ row.code }}</span>
        </template>
      </el-table-column>
      <el-table-column label="卡种" min-width="130" show-overflow-tooltip><template #default="{ row }">{{ row.card_type || '-' }}</template></el-table-column>
      <el-table-column label="状态" width="140">
        <template #default="{ row }">
          <el-tag :type="row.status === 'unused' ? 'success' : 'info'" size="small">
            {{ row.status === 'unused' ? '未使用' : '已使用' }}
          </el-tag>
          <el-tag v-if="row.frozen" type="danger" size="small">冻结</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="remark" label="备注" min-width="120" show-overflow-tooltip />
      <el-table-column prop="maker_name" label="制卡人" width="110" />
      <el-table-column label="批次" min-width="200" show-overflow-tooltip>
        <template #default="{ row }">
          <span v-if="row.batch_id" class="mono" style="cursor:pointer" title="点击复制批次" @click="copyText(row.batch_id, '批次')">{{ row.batch_id }}</span>
          <span v-else style="color:#bbb">-</span>
        </template>
      </el-table-column>
      <el-table-column label="制卡时间" width="160"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
      <el-table-column label="使用时间" width="160"><template #default="{ row }">{{ row.used_at ? fmt(row.used_at) : '-' }}</template></el-table-column>
      <el-table-column label="使用用户" min-width="160" show-overflow-tooltip>
        <template #default="{ row }"><span v-if="row.used_by" class="mono">{{ row.used_by }}</span><span v-else style="color:#bbb">-</span></template>
      </el-table-column>
      <el-table-column label="操作" :width="isMobile ? 96 : 150" fixed="right">
        <template #default="{ row }">
          <RowActions :actions="[
            { label: row.frozen ? '解冻' : '冻结', show: auth.can('freezeOp'), on: () => toggleFreeze(row) },
            { label: '删除', type: 'danger', show: auth.can('delCardAndUser') || row.status === 'unused', on: () => delCard(row) },
          ]" />
        </template>
      </el-table-column>
    </el-table>
    <el-pagination style="margin-top:14px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="cardTotal" :page-size="pageSize" :current-page="page" @current-change="(p) => { page = p; reloadCards() }" />
  </div>
</template>

<script setup>
import { ref, reactive, watch, onMounted } from 'vue'
import { Search, RefreshLeft, Document } from '@element-plus/icons-vue'
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

const blankQuery = () => ({ field: 'all', keyword: '', type_id: '', status: '', frozen: '', owner: '', ownerSub: false, batch_id: '', createdRange: null, usedRange: null })
const q = reactive(blankQuery())
const cards = ref([])
const cardTotal = ref(0)
const page = ref(1)
const pageSize = 20
const loadingCards = ref(false)
const types = ref([])
const subAgents = ref([])

async function loadTypes() {
  if (!appStore.currentAppId) return
  types.value = (await api.get('/api/card-types', { params: { app_id: appStore.currentAppId } })).items
}
async function loadSubAgents() {
  try { subAgents.value = (await api.get('/api/agents/subtree')).items } catch { subAgents.value = [] }
}
async function reloadCards() {
  if (!appStore.currentAppId) return
  loadingCards.value = true
  try {
    let r
    if (batchCodes.value) {
      r = await api.post('/api/cards/batch', { app_id: appStore.currentAppId, codes: batchCodes.value, page: page.value, pageSize })
      batchCount.value = r.queried || 0
    } else {
      const p = { app_id: appStore.currentAppId, page: page.value, pageSize, field: q.field }
      if (q.keyword) p.keyword = q.keyword
      if (q.type_id) p.type_id = q.type_id
      if (q.status) p.status = q.status
      if (q.frozen !== '') p.frozen = q.frozen
      if (q.batch_id) p.batch_id = q.batch_id
      if (q.owner) { p.owner = q.owner; if (q.ownerSub) p.ownerSub = '1' }
      if (q.createdRange?.length === 2) { p.createdRangeStart = toEpoch(q.createdRange[0]); p.createdRangeEnd = toEpoch(q.createdRange[1]) }
      if (q.usedRange?.length === 2) { p.usedRangeStart = toEpoch(q.usedRange[0]); p.usedRangeEnd = toEpoch(q.usedRange[1]) }
      r = await api.get('/api/cards', { params: p })
    }
    cards.value = r.items; cardTotal.value = r.total
  } finally { loadingCards.value = false }
}
function search() { batchCodes.value = ''; page.value = 1; reloadCards() }
function reset() { Object.assign(q, blankQuery()); batchCodes.value = ''; batchInput.value = ''; page.value = 1; reloadCards() }

const batchVisible = ref(false)
const batchInput = ref('')
const batchCodes = ref('')
const batchCount = ref(0)
function doBatch() {
  if (!batchInput.value.trim()) return ElMessage.warning('请粘贴卡号')
  batchCodes.value = batchInput.value
  batchVisible.value = false; page.value = 1; reloadCards()
}
function clearBatch() { batchCodes.value = ''; batchInput.value = ''; page.value = 1; reloadCards() }

async function toggleFreeze(row) {
  const frozen = row.frozen ? 0 : 1
  const verb = frozen ? '冻结' : '解冻'
  let freeze_user = false
  if (row.status === 'used') {
    try {
      await ElMessageBox.confirm(`此卡已被使用，是否同时${verb}使用它的用户？`, `${verb}卡密`,
        { confirmButtonText: `是，一起${verb}`, cancelButtonText: `否，仅此卡`, distinguishCancelAndClose: true, type: 'warning' })
      freeze_user = true
    } catch (a) { if (a === 'close') return; freeze_user = false }
  }
  await api.post('/api/cards/freeze', { app_id: appStore.currentAppId, id: row.id, frozen, freeze_user })
  ElMessage.success('已更新'); reloadCards()
}
async function delCard(row) {
  await ElMessageBox.confirm('确定删除该卡密？', '危险操作', { type: 'warning' })
  await api.post('/api/cards/delete', { app_id: appStore.currentAppId, id: row.id })
  ElMessage.success('已删除'); reloadCards()
}
function copyCode(code) { navigator.clipboard.writeText(code); ElMessage.success('已复制卡密') }
function copyText(t, label) { navigator.clipboard.writeText(t); ElMessage.success('已复制' + (label || '')) }

watch(() => appStore.currentAppId, () => { page.value = 1; loadTypes(); reloadCards() })
onMounted(() => { loadTypes(); loadSubAgents(); reloadCards() })
</script>
