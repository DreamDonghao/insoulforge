<script lang="ts" setup>
import {inject, onMounted, reactive, ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {numberError} from '../../utils/validation'
import type {ApiResponse, ExecutionConfig} from '../../vite-env.d'

const showToast = inject<(message: string, isError?: boolean) => void>('showToast')
const config = reactive<ExecutionConfig>({maxToolRounds: 8, routeWhileReplying: false, mcpToolPageSize: 10, mcpMaxToolsPerEnable: 4, mcpMaxToolsPerReply: 12})
const mcpFields = [
  {key: 'mcpToolPageSize', label: '查询每页工具数量', maximum: 50},
  {key: 'mcpMaxToolsPerEnable', label: '单次启用工具上限', maximum: 20},
  {key: 'mcpMaxToolsPerReply', label: '本次回复启用工具上限', maximum: 64}
] as const
const loading = ref(false)
const saving = ref(false)
const loadError = ref('')
const savedConfig = ref<string | null>(null)
const isDirty = (): boolean => savedConfig.value !== null && JSON.stringify(config) !== savedConfig.value
useUnsavedChanges(isDirty, () => saving.value)
const loadConfig = async (): Promise<void> => {
  if (loading.value || saving.value || !confirmDiscard(isDirty())) return
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
    config.routeWhileReplying = data.routeWhileReplying ?? false
    for (const field of mcpFields) {
      if (!Number.isInteger(data[field.key]) || data[field.key] < 1 || data[field.key] > field.maximum) throw new Error('MCP 数量配置无效')
      config[field.key] = data[field.key]
    }
    if (config.mcpMaxToolsPerEnable > config.mcpMaxToolsPerReply) throw new Error('单次启用上限不能大于本次回复上限')
    savedConfig.value = JSON.stringify(config)
  } catch (error) {
    loadError.value = error instanceof Error ? error.message : '无法连接管理服务'
  } finally {
    loading.value = false
  }
}

const {errors, fieldAttrs, validateForm} = useFormValidation(() => {
  const result: Record<string, string> = {'max-tool-rounds': numberError(config.maxToolRounds, 1, 100)}
  for (const field of mcpFields) result[field.key] = numberError(config[field.key], 1, field.maximum)
  if (config.mcpMaxToolsPerEnable > config.mcpMaxToolsPerReply) result.mcpMaxToolsPerEnable = '单次启用上限不能大于本次回复上限'
  return result
})

const saveConfig = async (): Promise<void> => {
  if (saving.value || loading.value || loadError.value || !validateForm()) return
  const submitted = JSON.stringify(config)
  saving.value = true
  try {
    const response = await fetch('/admin/api/execution-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: submitted
    })
    const data: ApiResponse = await response.json()
    if (!response.ok || !data.success) throw new Error(data.error || '保存执行配置失败')
    savedConfig.value = submitted
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
    <form v-else class="card" novalidate @submit.prevent="saveConfig">
      <div class="card-header">
        <h2 class="card-title">工具调用</h2>
      </div>
      <div class="form-group rounds-field">
        <label class="form-label" for="max-tool-rounds">最大迭代轮数</label>
        <input v-bind="fieldAttrs('max-tool-rounds', 'rounds-range')" v-model.number="config.maxToolRounds" :disabled="saving"
               class="form-input"
               type="number" min="1" max="100" step="1" required>
        <p id="rounds-range" class="form-hint">1～100 轮</p>
        <FieldError field="max-tool-rounds" :message="errors['max-tool-rounds']"/>
      </div>
      <h2 class="card-title mcp-heading">MCP 工具按需启用</h2>
      <div class="form-row mcp-fields">
        <div v-for="field in mcpFields" :key="field.key" class="form-group rounds-field">
          <label class="form-label" :for="field.key">{{ field.label }}</label>
          <input v-bind="fieldAttrs(field.key, field.key + '-hint')" v-model.number="config[field.key]" class="form-input" type="number" min="1"
                 :max="field.maximum" step="1" required :disabled="saving">
          <p :id="field.key + '-hint'" class="form-hint">1～{{ field.maximum }}</p>
          <FieldError :field="field.key" :message="errors[field.key]"/>
        </div>
      </div>
      <h2 class="card-title mcp-heading">群聊消息判断</h2>
      <div class="form-group">
        <label class="form-checkbox"><input v-model="config.routeWhileReplying" :disabled="saving" type="checkbox" aria-describedby="route-while-replying-hint">回复期间继续判断新消息（保存后生效）</label>
        <p id="route-while-replying-hint" class="form-hint">默认关闭。开启后，普通群消息在回复期间仍由 Jev 优先判断，无法确定时回退回复决策模型；需要回复的消息合并到下一轮，不打断当前回复，也不同时生成多条回复。会增加路由请求量。</p>
      </div>
      <button :disabled="saving" class="btn btn-primary" type="submit">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
.rounds-field {
  max-width: 360px;
}
.mcp-heading {margin: 24px 0 16px; padding-top: 20px; border-top: 1px solid var(--border)}
.mcp-fields { grid-template-columns: repeat(auto-fit, minmax(min(220px, 100%), 1fr)); }

.load-error {
  margin-bottom: 16px;
}

.invalid-range {
  color: var(--danger);
}
</style>
