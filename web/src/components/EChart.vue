<template>
  <div ref="el" :style="{ width: '100%', height: typeof height === 'number' ? height + 'px' : height }"></div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue'
import echarts from '../echarts'

const props = defineProps({
  option: { type: Object, required: true },
  height: { type: [Number, String], default: 300 },
})

const el = ref(null)
let chart = null
let ro = null

function render() {
  if (chart && props.option) chart.setOption(props.option, true)   // notMerge：完整替换，支持系列增减
}

onMounted(() => {
  chart = echarts.init(el.value)
  render()
  ro = new ResizeObserver(() => chart && chart.resize())
  ro.observe(el.value)
})

watch(() => props.option, render, { deep: true })

onBeforeUnmount(() => {
  if (ro) ro.disconnect()
  if (chart) { chart.dispose(); chart = null }
})
</script>
