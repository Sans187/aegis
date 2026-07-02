<template>
  <!-- 桌面端：行内按钮 -->
  <template v-if="!isMobile">
    <el-button v-for="(a, i) in visible" :key="i" size="small" :type="a.type" :plain="a.plain"
               :disabled="a.disabled" @click="a.on">{{ a.label }}</el-button>
  </template>
  <!-- 手机端：折叠成下拉菜单 -->
  <el-dropdown v-else trigger="click" @command="(i) => visible[i].on()">
    <el-button size="small" type="primary" plain>
      操作<el-icon style="margin-left:2px"><ArrowDown /></el-icon>
    </el-button>
    <template #dropdown>
      <el-dropdown-menu>
        <el-dropdown-item v-for="(a, i) in visible" :key="i" :command="i" :disabled="a.disabled"
                          :class="{ 'ra-danger': a.type === 'danger' && !a.disabled }">{{ a.label }}</el-dropdown-item>
      </el-dropdown-menu>
    </template>
  </el-dropdown>
</template>

<script setup>
import { computed } from 'vue'
import { ArrowDown } from '@element-plus/icons-vue'
import { useMobile } from '../composables/useMobile'

const props = defineProps({
  // [{ label, type?, plain?, show?(默认 true), on() }]
  actions: { type: Array, required: true },
})

const visible = computed(() => props.actions.filter((a) => a.show !== false))
const { isMobile } = useMobile()
</script>

<style>
.ra-danger { color: var(--el-color-danger); }
</style>
