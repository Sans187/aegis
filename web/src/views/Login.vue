<template>
  <div class="login-wrap">
    <div class="login-card">
      <h1>Aegis</h1>
      <div class="sub">验证系统管理后台</div>
      <el-form @submit.prevent="onSubmit">
        <el-form-item>
          <el-input v-model="username" size="large" placeholder="用户名" :prefix-icon="User" />
        </el-form-item>
        <el-form-item>
          <el-input
            v-model="password" size="large" type="password" show-password
            placeholder="密码" :prefix-icon="Lock" @keyup.enter="onSubmit"
          />
        </el-form-item>
        <el-button
          type="primary" size="large" style="width:100%" :loading="loading"
          @click="onSubmit"
        >
          登 录
        </el-button>
      </el-form>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { User, Lock } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import { useAuth } from '../stores/auth'

const router = useRouter()
const auth = useAuth()
const username = ref('')
const password = ref('')
const loading = ref(false)

async function onSubmit() {
  if (!username.value || !password.value) return ElMessage.warning('请输入用户名和密码')
  loading.value = true
  try {
    await auth.login(username.value, password.value)
    if (auth.mustChange) {
      ElMessage.warning('首次登录请先修改密码')
      router.push('/password')
    } else {
      router.push('/users')
    }
  } catch (e) { /* 拦截器已提示 */ } finally {
    loading.value = false
  }
}
</script>
