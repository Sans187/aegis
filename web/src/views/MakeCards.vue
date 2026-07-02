<template>
  <h2 class="page-title">制卡</h2>

  <div class="card" style="max-width:560px">
    <el-form label-width="90px">
      <el-form-item label="卡种">
        <el-select v-model="form.type_id" style="width:100%" placeholder="选择卡种">
          <el-option v-for="t in availableTypes" :key="t.id" :label="`${t.name}（${t.hours}小时，前缀 ${t.prefix}）`" :value="t.id" />
        </el-select>
      </el-form-item>
      <el-form-item label="数量"><el-input-number v-model="form.count" :min="1" :max="2000" /></el-form-item>
      <el-form-item label="备注"><el-input v-model="form.remark" placeholder="写到每张卡上" /></el-form-item>
      <el-form-item label="批次备注"><el-input v-model="form.note" placeholder="记录本批次用途" /></el-form-item>
      <el-form-item v-if="!my.super" label="余额">
        <div style="line-height:1.8">
          可用余额：<b style="color:#409eff">{{ money(my.available) }}</b>
          <template v-if="form.type_id">
            　·　单价 {{ money(unitPrice) }} × {{ form.count }} =
            <b :style="{ color: cost > my.available ? '#f56c6c' : '#67c23a' }">{{ money(cost) }}</b>
            <span v-if="cost > my.available" style="color:#f56c6c">（余额不足）</span>
          </template>
        </div>
      </el-form-item>
      <el-button type="primary" :icon="Plus" :loading="making" :disabled="!my.super && cost > my.available" @click="doMake">生成卡密</el-button>
      <span v-if="!types.length" style="color:#f56c6c;margin-left:12px">该软件还没有卡种，请先到「软件管理」添加</span>
      <span v-else-if="!availableTypes.length" style="color:#f56c6c;margin-left:12px">你没有可制作的卡种（未授权），请联系上级开通</span>
    </el-form>
  </div>

  <div v-if="resultCodes.length" class="card" style="max-width:560px;margin-top:16px">
    <div class="toolbar">
      <b>本次生成 {{ resultCodes.length }} 张</b>
      <div class="spacer" style="flex:1"></div>
      <el-button :icon="CopyDocument" @click="copyAll">复制全部</el-button>
      <el-button :icon="Download" @click="downloadTxt">导出 txt</el-button>
    </div>
    <el-input type="textarea" :rows="12" :model-value="resultCodes.join('\n')" readonly class="mono" />
  </div>

  <!-- 制卡批次列表 -->
  <div class="card" style="margin-top:16px">
    <div class="page-title" style="font-size:15px">制卡批次</div>
    <div class="toolbar">
      <el-select v-model="bq.scope" style="width:130px" @change="onScopeChange">
        <el-option label="仅自己" value="self" />
        <el-option label="所有代理" value="all" />
        <el-option label="指定代理" value="agent" />
      </el-select>
      <el-select v-if="bq.scope === 'agent'" v-model="bq.owner" placeholder="选择代理" style="width:180px" clearable filterable>
        <el-option v-for="a in subAgents" :key="a.id"
                   :label="(a.nickname ? a.nickname + ' / ' : '') + a.username + (a.self ? '（我）' : '')" :value="a.id" />
      </el-select>
      <el-checkbox v-if="bq.scope === 'agent'" v-model="bq.ownerSub" :disabled="!bq.owner" border>含下级</el-checkbox>
      <el-input v-model="bq.keyword" placeholder="批次ID / 备注" style="width:180px" clearable @keyup.enter="searchBatches" />
      <el-date-picker v-model="bq.createdRange" type="datetimerange" start-placeholder="起" end-placeholder="止" style="width:340px" />
      <el-button type="primary" :icon="Search" @click="searchBatches">查询</el-button>
      <el-button :icon="RefreshLeft" @click="resetBatches">重置</el-button>
    </div>
    <el-table :data="batches" v-loading="loadingBatches" stripe>
      <el-table-column label="批次ID" min-width="200">
        <template #default="{ row }"><span class="mono" style="cursor:pointer" title="点击复制" @click="copyText(row.id)">{{ row.id }}</span></template>
      </el-table-column>
      <el-table-column label="卡种" min-width="130" show-overflow-tooltip><template #default="{ row }">{{ row.type_name || '-' }}</template></el-table-column>
      <el-table-column label="数量" width="90"><template #default="{ row }">{{ row.count }}</template></el-table-column>
      <el-table-column prop="note" label="备注" min-width="120" show-overflow-tooltip />
      <el-table-column prop="maker_name" label="制卡人" width="120" />
      <el-table-column v-if="auth.isSuper" label="制卡IP" width="140">
        <template #default="{ row }">
          <span v-if="row.maker_ip" class="mono" style="cursor:pointer" title="点击复制" @click="copyText(row.maker_ip)">{{ row.maker_ip }}</span>
          <span v-else>-</span>
        </template>
      </el-table-column>
      <el-table-column label="制卡时间" width="170"><template #default="{ row }">{{ fmt(row.created_at) }}</template></el-table-column>
    </el-table>
    <el-pagination style="margin-top:14px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="batchTotal" :page-size="batchPageSize" :current-page="bpage" @current-change="(p) => { bpage = p; loadBatches() }" />
  </div>
</template>

<script setup>
import { ref, reactive, computed, watch, onMounted } from 'vue'
import { Plus, CopyDocument, Download, Search, RefreshLeft } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import api from '../api'
import { fmt, toEpoch } from '../utils/time'
import { useAppStore } from '../stores/app'
import { useAuth } from '../stores/auth'

const auth = useAuth()
const appStore = useAppStore()
const types = ref([])
const form = ref({ type_id: '', count: 1, remark: '', note: '' })
const making = ref(false)
const resultCodes = ref([])

// 余额账目 + 本次制卡花费
const my = ref({ super: false, balance: 0, available: 0 })
function money(n) { return Number(n || 0).toFixed(2) }
async function loadMy() {
  try { my.value = await api.get('/api/agents/my-balance') } catch { /* ignore */ }
}
// 只展示该代理被授权的卡种（与后端制卡鉴权一致：超管=全部；否则取 card_types[app] 的卡种）
const availableTypes = computed(() => {
  if (my.value.super || auth.isSuper) return types.value
  const ct = auth.agent?.card_types?.[appStore.currentAppId]
  if (!ct) return []
  const keys = Array.isArray(ct) ? ct : Object.keys(ct)
  return types.value.filter((t) => keys.includes(t.id))
})
const unitPrice = computed(() => {
  if (my.value.super) return 0
  const ct = auth.agent?.card_types?.[appStore.currentAppId]
  if (!ct || Array.isArray(ct)) return 0
  return Number(ct[form.value.type_id] || 0)
})
const cost = computed(() => unitPrice.value * (form.value.count || 0))

// 批次列表
const subAgents = ref([])
const blankBq = () => ({ scope: 'self', keyword: '', owner: '', ownerSub: false, createdRange: null })
const bq = reactive(blankBq())
const batches = ref([])
const batchTotal = ref(0)
const bpage = ref(1)
const batchPageSize = 20         // 每页显示 20 条
const loadingBatches = ref(false)

async function loadTypes() {
  if (!appStore.currentAppId) return
  types.value = (await api.get('/api/card-types', { params: { app_id: appStore.currentAppId } })).items
}
async function loadSubAgents() {
  try { subAgents.value = (await api.get('/api/agents/subtree')).items } catch { subAgents.value = [] }
}
async function loadBatches() {
  if (!appStore.currentAppId) return
  loadingBatches.value = true
  try {
    const p = { app_id: appStore.currentAppId, page: bpage.value, pageSize: batchPageSize, scope: bq.scope }
    if (bq.keyword) p.keyword = bq.keyword
    if (bq.scope === 'agent' && bq.owner) { p.owner = bq.owner; if (bq.ownerSub) p.ownerSub = '1' }
    if (bq.createdRange?.length === 2) { p.createdRangeStart = toEpoch(bq.createdRange[0]); p.createdRangeEnd = toEpoch(bq.createdRange[1]) }
    const r = await api.get('/api/card-batches', { params: p })
    batches.value = r.items; batchTotal.value = r.total
  } finally { loadingBatches.value = false }
}
function searchBatches() { bpage.value = 1; loadBatches() }
function resetBatches() { Object.assign(bq, blankBq()); bpage.value = 1; loadBatches() }
function onScopeChange() {
  if (bq.scope !== 'agent') { bq.owner = ''; bq.ownerSub = false }
  bpage.value = 1; loadBatches()
}

async function doMake() {
  if (!form.value.type_id) return ElMessage.warning('请选择卡种')
  making.value = true
  try {
    const r = await api.post('/api/cards/make', { app_id: appStore.currentAppId, ...form.value })
    resultCodes.value = r.codes || []
    ElMessage.success(`已生成 ${r.count} 张`)
    bpage.value = 1; loadBatches(); loadMy()   // 刷新批次列表与余额
  } finally { making.value = false }
}
function copyAll() { navigator.clipboard.writeText(resultCodes.value.join('\n')); ElMessage.success('已复制') }
function copyText(t) { navigator.clipboard.writeText(t); ElMessage.success('已复制') }
function downloadTxt() {
  const blob = new Blob([resultCodes.value.join('\r\n')], { type: 'text/plain' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = `cards_${appStore.currentAppId}_${Date.now()}.txt`
  a.click(); URL.revokeObjectURL(a.href)
}

watch(() => appStore.currentAppId, () => { resultCodes.value = []; form.value.type_id = ''; bpage.value = 1; loadTypes(); loadBatches() })
onMounted(() => { loadTypes(); loadSubAgents(); loadBatches(); loadMy() })
</script>
