import { ref, onMounted, onUnmounted } from 'vue'

// 响应式判断是否手机端（≤768px），用于折叠操作按钮等
export function useMobile() {
  const isMobile = ref(false)
  let mql
  const sync = () => { isMobile.value = mql.matches }
  onMounted(() => { mql = window.matchMedia('(max-width: 768px)'); sync(); mql.addEventListener('change', sync) })
  onUnmounted(() => { mql && mql.removeEventListener('change', sync) })
  return { isMobile }
}
