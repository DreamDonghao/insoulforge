<script lang="ts" setup>
import {computed, inject, onMounted, reactive, ref} from 'vue'
import type {ApiResponse, ExecutionConfig} from '../../vite-env.d'

const showToast = inject<(message: string, isError?: boolean) => void>('showToast')
const config = reactive<ExecutionConfig>({maxToolRounds: 8})
const loading = ref(true)
const saving = ref(false)
const loadError = ref('')
const validRounds = computed(() => Number.isInteger(config.maxToolRounds) &&
    config.maxToolRounds >= 1 && config.maxToolRounds <= 100)

const loadConfig = async (): Promise<void> => {
  loading.value = true
  loadError.value = ''
  try {
    const response = await fetch('/admin/api/execution-config')
    if (!response.ok) throw new Error('加载执行配置失败')
    const data: ExecutionConfig = await response.json()
    if (!Number.isInteger(data.maxToolRounds) || data.maxToolRounds < 1 || data.maxToolRounds > 100) {
      throw new Error('执行配置数据无效')
    }
    config.maxToolRounds = data.maxToolRounds
  } catch (error) {
    loadError.value = error instanceof Error ? error.message : '无法连接管理服务'
  } finally {
    loading.value = false
  }
}

const saveConfig = async (): Promise<void> => {
  if (!validRounds.value || saving.value || loading.value || loadError.value) return
  saving.value = true
  try {
    const response = await fetch('/admin/api/execution-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(config)
    })
    const data: ApiResponse = await response.json()
    if (!response.ok || !data.success) throw new Error(data.error || '保存执行配置失败')
    showToast?.('执行配置已保存')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '无法连接管理服务', true)
  } finally {
    saving.value = false
  }
}

onMounted(loadConfig)
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">执行配置</h1>
    </div>

    <div v-if="loading" class="card" role="status">加载中...</div>
    <div v-else-if="loadError" class="card" role="alert">
      <p class="load-error">{{ loadError }}</p>
      <button class="btn btn-secondary" type="button" @click="loadConfig">重新加载</button>
    </div>
    <form v-else class="card" @submit.prevent="saveConfig">
      <div class="card-header">
        <h2 class="card-title">工具调用</h2>
      </div>
      <div class="form-group rounds-field">
        <label class="form-label" for="max-tool-rounds">最大迭代轮数</label>
        <input id="max-tool-rounds" v-model.number="config.maxToolRounds" :disabled="saving"
               :aria-invalid="!validRounds" aria-describedby="rounds-range" class="form-input"
               type="number" min="1" max="100" step="1" required>
        <p id="rounds-range" :class="{'invalid-range': !validRounds}" class="form-hint">1～100 轮</p>
      </div>
      <button :disabled="saving || !validRounds" class="btn btn-primary" type="submit">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
.rounds-field {
  max-width: 360px;
}

.load-error {
  margin-bottom: 16px;
}

.invalid-range {
  color: var(--danger);
}
</style>
