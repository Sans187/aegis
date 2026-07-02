<template>
  <div style="max-width:480px;margin:0 auto">
    <h2 class="page-title">修改密码</h2>
    <el-alert
      v-if="auth.mustChange"
      title="出于安全考虑，请先设置一个新密码后再使用系统。"
      type="warning" :closable="false" style="margin-bottom:16px"
    />
    <div class="card">
      <el-form label-width="90px" @submit.prevent="onSubmit">
        <el-form-item label="原密码">
          <el-input v-model="oldP" type="password" show-password placeholder="当前密码" />
        </el-form-item>
        <el-form-item label="新密码">
          <el-input v-model="newP" type="password" show-password placeholder="至少 8 位" />
        </el-form-item>
        <el-form-item label="确认新密码">
          <el-input v-model="confirm" type="password" show-password />
        </el-form-item>
        <el-button type="primary" :loading="loading" @click="onSubmit">保存</el-button>
      </el-form>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import api from '../api'
import { useAuth } from '../stores/auth'

const router = useRouter()
const auth = useAuth()
const oldP = ref('')
const newP = ref('')
const confirm = ref('')
const loading = ref(false)

async function onSubmit() {
  if (newP.value.length < 8) return ElMessage.warning('新密码至少 8 位')
  if (newP.value !== confirm.value) return ElMessage.warning('两次输入不一致')
  loading.value = true
  try {
    const r = await api.post('/api/auth/change-password', {
      oldPassword: oldP.value, newPassword: newP.value,
    })
    if (r.token) auth.setToken(r.token)
    auth.mustChange = false
    if (auth.agent) auth.agent.must_change_password = false
    ElMessage.success('密码已更新')
    router.push('/users')
  } catch (e) { /* 拦截器已提示 */ } finally {
    loading.value = false
  }
}
</script>
