<template>
  <div style="max-width:480px">
    <h2 class="page-title">安全设置</h2>
    <div class="card">
      <div style="margin-bottom:16px">
        <el-tag :type="status.set ? 'success' : 'info'">
          安全密码{{ status.set ? '已设置' : '未设置' }}
        </el-tag>
        <span style="color:#8a90a2;font-size:13px;margin-left:8px">
          用于添加/删除软件等敏感操作的二次确认（二级密码）
        </span>
      </div>
      <el-form label-width="100px" @submit.prevent="save">
        <el-form-item label="登录密码">
          <el-input v-model="loginPassword" type="password" show-password placeholder="用当前登录密码确认身份" />
        </el-form-item>
        <el-form-item :label="status.set ? '新安全密码' : '安全密码'">
          <el-input v-model="securityPassword" type="password" show-password placeholder="至少 6 位" />
        </el-form-item>
        <el-form-item label="确认">
          <el-input v-model="confirm" type="password" show-password />
        </el-form-item>
        <el-button type="primary" :loading="loading" @click="save">
          {{ status.set ? '修改安全密码' : '设置安全密码' }}
        </el-button>
      </el-form>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import api from '../api'

const status = ref({ set: false, isSuper: false })
const loginPassword = ref('')
const securityPassword = ref('')
const confirm = ref('')
const loading = ref(false)

async function load() { status.value = await api.get('/api/security/status') }

async function save() {
  if (securityPassword.value.length < 6) return ElMessage.warning('安全密码至少 6 位')
  if (securityPassword.value !== confirm.value) return ElMessage.warning('两次输入不一致')
  if (!loginPassword.value) return ElMessage.warning('请输入登录密码确认')
  loading.value = true
  try {
    await api.post('/api/security/set', {
      loginPassword: loginPassword.value, securityPassword: securityPassword.value,
    })
    ElMessage.success('安全密码已保存')
    loginPassword.value = securityPassword.value = confirm.value = ''
    load()
  } finally { loading.value = false }
}

onMounted(load)
</script>
