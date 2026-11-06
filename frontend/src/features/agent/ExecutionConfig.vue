<script lang="ts" setup>
import {computed, inject, onMounted, reactive, ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
import NavIcon from '../../components/NavIcon.vue'
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
const hintsDismissed = ref(false)
const expandedHelp = ref('')
const toggleHelp = (key: string): void => {
  const closing = expandedHelp.value === key
  expandedHelp.value = closing ? '' : key
  hintsDismissed.value = closing
}
const savedConfig = ref<string | null>(null)
const isDirty = (): boolean => savedConfig.value !== null && JSON.stringify(config) !== savedConfig.value
const hasUnsavedChanges = computed(isDirty)
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
    config.defaultPrivateEnabled = data.defaultPrivateEnabled ?? false
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
    <form v-else class="execution-form" :class="{'hints-dismissed': hintsDismissed}" novalidate
          @keydown.esc="hintsDismissed = true; expandedHelp = ''" @submit.prevent="saveConfig">
      <section class="execution-section">
        <h2 class="card-title section-heading">工具调用</h2>
        <div class="form-group rounds-field" @focusin="hintsDismissed = false">
          <label class="form-label" for="max-tool-rounds">最大迭代轮数</label>
          <input v-bind="fieldAttrs('max-tool-rounds', 'rounds-range')" v-model.number="config.maxToolRounds" :disabled="saving"
                 class="form-input"
                 type="number" min="1" max="100" step="1" required>
          <p id="rounds-range" class="form-hint" role="tooltip">1～100 轮</p>
          <FieldError field="max-tool-rounds" :message="errors['max-tool-rounds']"/>
        </div>
      </section>
      <section class="execution-section">
        <h2 class="card-title section-heading">MCP 工具按需启用</h2>
        <div class="form-row mcp-fields">
          <div v-for="field in mcpFields" :key="field.key" class="form-group rounds-field" @focusin="hintsDismissed = false">
            <label class="form-label" :for="field.key">{{ field.label }}</label>
            <input v-bind="fieldAttrs(field.key, field.key + '-hint')" v-model.number="config[field.key]" class="form-input" type="number" min="1"
                   :max="field.maximum" step="1" required :disabled="saving">
            <p :id="field.key + '-hint'" class="form-hint" role="tooltip">1～{{ field.maximum }}</p>
            <FieldError :field="field.key" :message="errors[field.key]"/>
          </div>
        </div>
      </section>
      <section class="execution-section">
        <h2 class="card-title section-heading">群聊消息判断</h2>
        <div class="form-group setting-row">
          <div class="setting-description">
            <div class="setting-name">
              <label for="route-enabled">回复期间继续判断新消息</label>
              <span class="setting-help" :class="{'is-expanded': expandedHelp === 'route'}"
                    @mouseenter="hintsDismissed = false" @focusin="hintsDismissed = false" @focusout="expandedHelp = ''">
                <button type="button" class="help-trigger" aria-label="查看回复期间继续判断新消息的详细说明"
                        aria-describedby="route-details" @click="toggleHelp('route')">
                  <NavIcon name="info" aria-hidden="true"/>
                </button>
                <span id="route-details" class="help-details" role="tooltip">默认关闭。开启后，普通群消息在回复期间仍由 Jev 优先判断，无法确定时回退回复决策模型；需要回复的消息合并到下一轮，不打断当前回复，也不同时生成多条回复。</span>
              </span>
            </div>
            <p id="route-while-replying-hint" class="setting-summary">开启后会增加请求量。</p>
          </div>
          <input id="route-enabled" v-model="config.routeWhileReplying" :disabled="saving" type="checkbox" aria-describedby="route-while-replying-hint">
        </div>
      </section>
      <section class="execution-section">
        <h2 class="card-title section-heading">私聊</h2>
        <div class="form-group setting-row">
          <div class="setting-description">
            <div class="setting-name">
              <label for="private-enabled">新私聊默认启用</label>
              <span class="setting-help" :class="{'is-expanded': expandedHelp === 'private'}"
                    @mouseenter="hintsDismissed = false" @focusin="hintsDismissed = false" @focusout="expandedHelp = ''">
                <button type="button" class="help-trigger" aria-label="查看新私聊默认启用的详细说明"
                        aria-describedby="private-details" @click="toggleHelp('private')">
                  <NavIcon name="info" aria-hidden="true"/>
                </button>
                <span id="private-details" class="help-details" role="tooltip">仅影响首次收到消息的新私聊，不改变已有会话的启用状态。</span>
              </span>
            </div>
            <p id="private-default-hint" class="setting-summary">仅影响新私聊。</p>
          </div>
          <input id="private-enabled" v-model="config.defaultPrivateEnabled" :disabled="saving" type="checkbox" aria-describedby="private-default-hint">
        </div>
      </section>
      <div class="save-actions">
        <button :disabled="saving" class="btn btn-primary" type="submit">
          {{ saving ? '保存中...' : '保存配置' }}
        </button>
        <p class="save-hint" :class="{'has-changes': hasUnsavedChanges}" role="status">{{ hasUnsavedChanges ? '有未保存的修改' : '修改后需保存' }}</p>
      </div>
    </form>
  </div>
</template>

<style scoped src="../../components/fieldHelp.css"/>

<style scoped>
.execution-form {
  display: grid;
  gap: 16px;
}

.execution-section {
  min-width: 0;
  padding: 20px 24px;
  background: var(--card-bg);
  border-radius: var(--radius-md);
}

.section-heading {
  margin-bottom: 12px;
  color: var(--text-secondary);
}

.execution-section .form-group {
  position: relative;
  margin-bottom: 0;
}

.rounds-field .form-hint {
  visibility: hidden;
  color: var(--text-secondary);
}

.execution-form:not(.hints-dismissed) .rounds-field:focus-within > .form-hint {
  visibility: visible;
}

.setting-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
}

.setting-description {
  min-width: 0;
}

.setting-name {
  display: flex;
  align-items: center;
  gap: 8px;
}

.setting-name label {
  min-width: 0;
  cursor: pointer;
}

.setting-row > input {
  flex-shrink: 0;
}

.setting-help {
  display: inline-flex;
  flex-shrink: 0;
}

.execution-form:not(.hints-dismissed) .setting-help:hover .help-details,
.execution-form:not(.hints-dismissed) .setting-help:focus-within .help-details,
.execution-form:not(.hints-dismissed) .setting-help.is-expanded .help-details {
  visibility: visible;
  transition-delay: 0s;
}

.setting-summary,
.save-hint {
  margin: 4px 0 0;
  font-size: 12px;
  color: var(--text-secondary);
}

.save-actions {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px 12px;
}

.save-hint {
  margin: 0;
}

.save-hint.has-changes {
  color: var(--warning);
  font-weight: 500;
}

.rounds-field {
  display: grid;
  grid-template-columns: minmax(0, 220px) minmax(0, 160px) minmax(0, 1fr);
  align-items: center;
  gap: 8px 16px;
}

.rounds-field .form-label,
.rounds-field .form-hint {
  margin: 0;
}

.rounds-field .form-input {
  height: 36px;
  padding: 6px 10px;
  line-height: 22px;
  background: color-mix(in srgb, var(--input-bg) 75%, var(--card-bg));
}

.rounds-field :deep(.form-error) {
  grid-column: 2 / -1;
  margin: 0;
}

.mcp-fields {
  grid-template-columns: minmax(0, 1fr);
  gap: 12px;
}

.mcp-fields > .rounds-field + .rounds-field::before {
  content: '';
  position: absolute;
  top: -6px;
  inset-inline-start: 0;
  width: min(396px, 100%);
  border-top: 1px solid color-mix(in srgb, var(--border) 35%, transparent);
  pointer-events: none;
}

@media (max-width: 600px) {
  .rounds-field {
    grid-template-columns: minmax(0, 1fr) minmax(0, 120px);
  }

  .rounds-field .form-hint {
    display: none;
  }

  .execution-form:not(.hints-dismissed) .rounds-field:focus-within > .form-hint {
    display: block;
    grid-column: 1 / -1;
  }

  .rounds-field :deep(.form-error) {
    grid-column: 1 / -1;
  }
}

.load-error {
  margin-bottom: 16px;
}

</style>
