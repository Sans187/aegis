<template>
  <h2 class="page-title">数据总览</h2>

  <!-- 今日数据 -->
  <div class="page-title" style="font-size:14px;margin:4px 0 8px;color:#888">今日数据</div>
  <div class="stat-row">
    <div class="stat"><div class="n" style="color:#409eff">{{ today.registrations || 0 }}</div><div class="l">今日注册</div></div>
    <div class="stat"><div class="n" style="color:#67c23a">{{ today.activations || 0 }}</div><div class="l">今日激活</div></div>
    <div class="stat"><div class="n" style="color:#f56c6c">{{ today.frozen || 0 }}</div><div class="l">今日冻结</div></div>
    <div class="stat"><div class="n" style="color:#e6a23c">¥{{ money(today.sales) }}</div><div class="l">今日销售额</div></div>
  </div>

  <!-- 旗下代理销售（可选日期，含卡种拆分） -->
  <div class="card" style="margin-bottom:16px">
    <div class="sale-head">
      <div>
        <div class="page-title" style="font-size:15px;margin:0">旗下代理销售</div>
        <div style="color:#aaa;font-size:12px;margin-top:2px">本人及全部下级代理，各代理只统计本人名下（不含其下级）· 共 {{ filteredAgents.length }} 个</div>
      </div>
      <div class="sale-tools">
        <el-select v-model="selectedAgents" multiple filterable clearable collapse-tags collapse-tags-tooltip
          placeholder="全部代理" style="min-width:200px;max-width:340px" @change="agPage = 1">
          <el-option v-for="a in daySales.agents" :key="a.username"
            :label="(a.nickname ? a.nickname + ' / ' : '') + a.username" :value="a.username" />
        </el-select>
        <el-date-picker v-model="selectedDate" type="date" value-format="YYYY-MM-DD" :clearable="false"
          :disabled-date="disableFuture" style="width:150px" @change="onDateChange" />
        <div class="sale-total">合计 <b>¥{{ money(shownTotal) }}</b></div>
      </div>
    </div>
    <div class="sale-list" v-loading="salesLoading">
      <div v-for="(a, i) in pagedAgents" :key="a.username" class="sale-item">
        <div class="sale-main">
          <div class="rank" :class="{ top: agStart + i < 3 }">{{ agStart + i + 1 }}</div>
          <div class="who">
            <div class="nm">{{ a.nickname || a.username }}</div>
            <div class="un mono">{{ a.username }}</div>
          </div>
          <div class="metrics">
            <div class="m"><span class="mv" style="color:#67c23a">{{ a.activations }}</span><span class="ml">激活</span></div>
            <div class="m"><span class="mv" style="color:#f56c6c">{{ a.frozen }}</span><span class="ml">冻结</span></div>
            <div class="m"><span class="mv" style="color:#e6a23c">¥{{ money(a.sales) }}</span><span class="ml">销售额</span></div>
          </div>
        </div>
        <div class="types" v-if="a.types && a.types.length">
          <span v-for="t in a.types" :key="t.name" class="tchip" :title="`销售额 ¥${money(t.sales)}`">{{ t.name }} <b>×{{ t.count }}</b></span>
        </div>
      </div>
      <div v-if="!filteredAgents.length" class="sale-empty">该日暂无销售</div>
    </div>
    <el-pagination v-if="filteredAgents.length > agSize" style="margin-top:12px;justify-content:flex-end" layout="total, prev, pager, next"
      :total="filteredAgents.length" :page-size="agSize" :current-page="agPage" @current-change="(p) => agPage = p" />
  </div>

  <!-- 汇总 -->
  <div class="page-title" style="font-size:14px;margin:14px 0 8px;color:#888">累计（所有软件）</div>
  <div class="stat-row">
    <div class="stat"><div class="n">{{ totals.apps || 0 }}</div><div class="l">软件数</div></div>
    <div class="stat"><div class="n">{{ totals.cards_total || 0 }}</div><div class="l">卡密总数</div></div>
    <div class="stat"><div class="n" style="color:#16a34a">{{ totals.cards_unused || 0 }}</div><div class="l">未使用卡</div></div>
    <div class="stat"><div class="n">{{ totals.users_total || 0 }}</div><div class="l">用户总数</div></div>
    <div class="stat"><div class="n" style="color:#6366f1">{{ totals.users_online || 0 }}</div><div class="l">在线用户</div></div>
  </div>

  <!-- 每日注册柱状图 -->
  <div class="card" style="margin-bottom:16px">
    <div class="page-title" style="font-size:15px">每日注册人数（近 14 天）</div>
    <EChart :option="regOption" :height="260" />
  </div>

  <!-- 近几天每小时激活人数（按天叠加曲线） -->
  <div class="card" style="margin-bottom:16px">
    <div class="hr-head">
      <div>
        <div class="page-title" style="font-size:15px;margin:0">近几天每小时激活人数</div>
        <div style="color:#aaa;font-size:12px;margin-top:2px">悬停看具体数字 · 点图例可切换某天 · 底部可拖动缩放</div>
      </div>
      <el-radio-group v-model="dayRange" size="small">
        <el-radio-button :value="10">10天</el-radio-button>
        <el-radio-button :value="7">7天</el-radio-button>
        <el-radio-button :value="5">5天</el-radio-button>
        <el-radio-button :value="3">3天</el-radio-button>
      </el-radio-group>
    </div>
    <EChart :option="hourOption" :height="360" />
  </div>

  <!-- 各软件明细 -->
  <div class="card">
    <div class="page-title" style="font-size:15px">各软件明细</div>
    <el-table :data="items" v-loading="loading" stripe>
      <el-table-column prop="app_id" label="软件ID" width="120"><template #default="{ row }"><span class="mono">{{ row.app_id }}</span></template></el-table-column>
      <el-table-column prop="name" label="软件名称" min-width="160" />
      <el-table-column prop="cards_total" label="卡密总数" width="110" />
      <el-table-column prop="cards_unused" label="未使用" width="100" />
      <el-table-column prop="cards_used" label="已使用" width="100" />
      <el-table-column prop="users_total" label="用户数" width="100" />
      <el-table-column label="在线" width="90">
        <template #default="{ row }"><el-tag size="small" :type="row.users_online ? '' : 'info'">{{ row.users_online }}</el-tag></template>
      </el-table-column>
    </el-table>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import api from '../api'
import EChart from '../components/EChart.vue'

const items = ref([])
const totals = ref({})
const today = ref({})
const daily = ref([])
const hourly = ref([])
const loading = ref(false)

// ---- 旗下代理销售（按日期，含卡种拆分）----
function todayStr() {
  const d = new Date()
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`
}
const selectedDate = ref(todayStr())
const daySales = ref({ date: '', total_sales: 0, agents: [] })
const salesLoading = ref(false)
const selectedAgents = ref([])   // 指定代理（用户名）多选，空=全部
const agPage = ref(1)
const agSize = 8
const agStart = computed(() => (agPage.value - 1) * agSize)
const filteredAgents = computed(() => {
  const all = daySales.value.agents
  if (!selectedAgents.value.length) return all
  const set = new Set(selectedAgents.value)
  return all.filter((a) => set.has(a.username))
})
const shownTotal = computed(() => {
  if (!selectedAgents.value.length) return daySales.value.total_sales
  return filteredAgents.value.reduce((s, a) => s + (Number(a.sales) || 0), 0)
})
const pagedAgents = computed(() => filteredAgents.value.slice(agStart.value, agStart.value + agSize))
function disableFuture(date) { return date.getTime() > Date.now() }
function money(v) {
  const n = Number(v) || 0
  return n.toLocaleString('zh-CN', { minimumFractionDigits: 2, maximumFractionDigits: 2 })
}
async function loadAgentSales(date) {
  salesLoading.value = true
  try {
    const r = await api.get('/api/stats/agent-sales', { params: { date } })
    daySales.value = { date: r.date, total_sales: r.total_sales || 0, agents: r.agents || [] }
    agPage.value = 1
  } finally { salesLoading.value = false }
}
function onDateChange() { loadAgentSales(selectedDate.value) }

// ---- 每日注册柱状图（ECharts，柱顶显示数字，悬停高亮）----
const regOption = computed(() => ({
  grid: { left: 8, right: 14, top: 30, bottom: 6, containLabel: true },
  tooltip: {
    trigger: 'axis', axisPointer: { type: 'shadow' },
    formatter: (p) => `${p[0].axisValue}<br/>注册 <b>${p[0].data}</b> 人`,
  },
  xAxis: {
    type: 'category', data: daily.value.map((d) => d.date),
    axisTick: { show: false }, axisLine: { lineStyle: { color: '#e5e7eb' } },
    axisLabel: { color: '#999', fontSize: 11 },
  },
  yAxis: {
    type: 'value', minInterval: 1,
    axisLabel: { color: '#bbb' }, splitLine: { lineStyle: { color: '#f0f2f5' } },
  },
  series: [{
    name: '注册', type: 'bar', data: daily.value.map((d) => d.registrations || 0),
    barMaxWidth: 28, itemStyle: { color: '#409eff', borderRadius: [3, 3, 0, 0] },
    emphasis: { itemStyle: { color: '#66b1ff' } },
    label: { show: true, position: 'top', color: '#888', fontSize: 11, formatter: (p) => (p.value > 0 ? p.value : '') },
  }],
}))

// ---- 近几天每小时激活曲线（ECharts，悬停看数值、图例可切换、可缩放）----
const dayRange = ref(3)
const palette = ['#fa8c16', '#1890ff', '#722ed1', '#52c41a', '#eb2f96', '#faad14', '#2f54eb', '#13a8a8', '#a0d911']
const hourLabels = Array.from({ length: 24 }, (_, h) => `${h}:00`)
const hourOption = computed(() => {
  const days = hourly.value.slice(-dayRange.value)
  const series = days.map((d, idx) => {
    const isToday = !!d.today
    return {
      name: isToday ? '今天' : d.date,
      type: 'line', smooth: true, symbol: 'circle', symbolSize: isToday ? 6 : 4,
      showSymbol: false, connectNulls: false,
      z: isToday ? 10 : idx + 1,
      lineStyle: { width: isToday ? 3.5 : 2 },
      itemStyle: { color: isToday ? '#13c2c2' : palette[idx % palette.length] },
      emphasis: { focus: 'series' },
      label: isToday
        ? { show: true, position: 'top', fontSize: 10, color: '#13c2c2', formatter: (p) => (p.value > 0 ? p.value : '') }
        : { show: false },
      data: hourLabels.map((_, h) => {
        const arr = d.hours || []
        return h < arr.length ? arr[h] : null
      }),
    }
  })
  return {
    grid: { left: 8, right: 16, top: 40, bottom: 60, containLabel: true },
    tooltip: { trigger: 'axis', axisPointer: { type: 'line' } },
    legend: { type: 'scroll', top: 4, left: 'center', itemWidth: 18, itemHeight: 8, textStyle: { fontSize: 12, color: '#666' } },
    dataZoom: [
      { type: 'inside', zoomOnMouseWheel: false, moveOnMouseWheel: false },
      { type: 'slider', height: 18, bottom: 14, borderColor: 'transparent', fillerColor: 'rgba(64,158,255,0.12)' },
    ],
    xAxis: {
      type: 'category', boundaryGap: false, data: hourLabels,
      axisLine: { lineStyle: { color: '#e5e7eb' } }, axisLabel: { color: '#bbb', fontSize: 11 },
    },
    yAxis: {
      type: 'value', minInterval: 1,
      axisLabel: { color: '#bbb' }, splitLine: { lineStyle: { color: '#f0f2f5', type: 'dashed' } },
    },
    series: series.length ? series : [{ type: 'line', data: [] }],
  }
})

async function load() {
  loading.value = true
  try {
    const r = await api.get('/api/stats')
    items.value = r.items
    totals.value = r.totals
    today.value = r.today || {}
    daily.value = r.daily || []
    hourly.value = r.hourly || []
  } finally { loading.value = false }
}
onMounted(() => { load(); loadAgentSales(selectedDate.value) })
</script>

<style scoped>
.hr-head { display: flex; align-items: flex-start; justify-content: space-between; flex-wrap: wrap; gap: 10px; margin-bottom: 4px; }

/* 旗下代理销售 */
.sale-head { display: flex; align-items: flex-start; justify-content: space-between; flex-wrap: wrap; gap: 10px; margin-bottom: 12px; }
.sale-tools { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
.sale-total { font-size: 13px; color: #888; white-space: nowrap; }
.sale-total b { color: #e6a23c; font-size: 16px; margin-left: 4px; }
.sale-list { display: flex; flex-direction: column; }
.sale-item { display: flex; flex-direction: column; gap: 6px; padding: 10px 4px; border-bottom: 1px solid #f2f3f5; }
.sale-item:last-child { border-bottom: none; }
.sale-main { display: flex; align-items: center; gap: 12px; }
.sale-main .rank { flex: 0 0 26px; height: 26px; line-height: 26px; text-align: center; border-radius: 50%;
  background: #f0f2f5; color: #999; font-size: 13px; font-weight: 700; }
.sale-main .rank.top { background: #fff4e6; color: #e6a23c; }
.sale-main .who { flex: 1 1 auto; min-width: 0; }
.sale-main .who .nm { font-weight: 600; color: #303133; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.sale-main .who .un { font-size: 12px; color: #b0b3b8; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.sale-main .metrics { flex: 0 0 auto; display: flex; gap: 22px; }
.sale-main .metrics .m { display: flex; flex-direction: column; align-items: flex-end; min-width: 56px; }
.sale-main .metrics .mv { font-size: 16px; font-weight: 700; line-height: 1.1; }
.sale-main .metrics .ml { font-size: 11px; color: #aaa; margin-top: 2px; }
.types { display: flex; flex-wrap: wrap; gap: 6px; padding-left: 38px; }
.tchip { display: inline-flex; align-items: center; gap: 3px; font-size: 12px; color: #555;
  background: #f4f6f9; border: 1px solid #e9edf2; border-radius: 10px; padding: 1px 9px; cursor: default; }
.tchip b { color: #409eff; font-weight: 700; }
.sale-empty { text-align: center; color: #bbb; padding: 24px 0; font-size: 14px; }
@media (max-width: 768px) {
  .sale-main { flex-wrap: wrap; gap: 8px; }
  .sale-main .metrics { width: 100%; justify-content: space-between; gap: 8px; padding-left: 38px; }
  .sale-main .metrics .m { align-items: flex-start; min-width: 0; }
  .types { padding-left: 0; }
}
</style>
