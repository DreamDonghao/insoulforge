<script lang="ts" setup>
import NavIcon from '../../components/NavIcon.vue'
import {computed, inject, reactive, ref, type Ref, watch} from 'vue'
import FieldError from '../../components/FieldError.vue'
import FieldHelp from '../../components/FieldHelp.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {I32_MAX, numberError} from '../../utils/validation'
import {useUnsavedChanges} from '../../composables/useUnsavedChanges'
import type {ApiResponse, LLMConfig} from '../../vite-env'

const props = defineProps<{name: string, initialConfig: LLMConfig, title?: string}>()
const hasParameters = computed(() => !['embedding', 'imageGeneration'].includes(props.name))
const hasGenerationParameters = computed(() => hasParameters.value && props.name !== 'jev')
const supportsPromptCacheKey = computed(() => ['executor', 'executorFallback'].includes(props.name))
const emit = defineEmits<{saved: [name: string, config: LLMConfig]}>()
const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const llmConfig = reactive<LLMConfig>({...props.initialConfig})
watch(() => llmConfig.apiType, type => {
  if (props.name !== 'imageGeneration') return
  if (type === 'openai' && llmConfig.path === '/images') llmConfig.path = '/images/generations'
  if (type === 'openrouter' && llmConfig.path === '/images/generations') llmConfig.path = '/images'
})
const saving: Ref<boolean> = ref(false)
const savedConfig = ref(JSON.stringify(props.initialConfig))
const dirty = computed(() => JSON.stringify(llmConfig) !== savedConfig.value)
useUnsavedChanges(() => dirty.value, () => saving.value)
defineExpose({dirty, saving})
const showApiKey: Ref<boolean> = ref(false)
const rangeHintsDismissed = ref(false)
watch(() => props.initialConfig, config => {
  const preserveDraft = dirty.value || saving.value
  savedConfig.value = JSON.stringify(config)
  if (!preserveDraft) Object.assign(llmConfig, config)
})
type NumericModelKey = 'timeoutSeconds' | 'maxRetries' | 'maxTokens' | 'temperature' | 'topP' | 'minConfidence'
interface NumericModelField {key: NumericModelKey, label: string, hint: string, help: string, minimum: number, maximum: number, step: number | 'any'}
const requestFields: NumericModelField[] = [
  {key: 'timeoutSeconds', label: '请求超时（秒）', hint: '1～3600 秒，整数。', help: '每次尝试独立计时，保存后对后续请求生效。', minimum: 1, maximum: 3600, step: 1},
  {key: 'maxRetries', label: '失败重试次数', hint: '0～10 次，整数。', help: '不包含首次请求；0 表示不重试，每次重试间隔 1 秒。', minimum: 0, maximum: 10, step: 1},
]
const generationFields: NumericModelField[] = [
  {key: 'maxTokens', label: '最大输出 Token', hint: '1～2147483647 Token，整数。', help: '实际输出上限受模型支持范围限制。', minimum: 1, maximum: I32_MAX, step: 1},
  {key: 'temperature', label: '温度（Temperature）', hint: '0～2，允许小数。', help: '具体支持范围以模型为准。', minimum: 0, maximum: 2, step: 'any'},
  {key: 'topP', label: '采样范围（Top P）', hint: '0～1，允许小数。', help: '', minimum: 0, maximum: 1, step: 'any'},
]
const decisionFields: NumericModelField[] = [
  {key: 'minConfidence', label: '最低置信度', hint: '0～1，允许小数。', help: '低于此值时由 Router LLM 判断，默认 0.60', minimum: 0, maximum: 1, step: 'any'},
]
const parameterFields = computed(() => hasGenerationParameters.value ? generationFields : props.name === 'jev' ? decisionFields : [])

const fieldId = (key: string): string => `model-${props.name}-${key}`
const {errors, fieldAttrs, validateForm} = useFormValidation(() => {
  const result: Record<string, string> = {
    [fieldId('timeoutSeconds')]: numberError(llmConfig.timeoutSeconds, 1, 3600),
    [fieldId('maxRetries')]: numberError(llmConfig.maxRetries, 0, 10)
  }
  if (hasGenerationParameters.value) {
    result[fieldId('maxTokens')] = numberError(llmConfig.maxTokens, 1, I32_MAX)
    result[fieldId('temperature')] = numberError(llmConfig.temperature, 0, 2, false)
    result[fieldId('topP')] = numberError(llmConfig.topP, 0, 1, false)
  }
  if (props.name === 'jev') result[fieldId('minConfidence')] = numberError(llmConfig.minConfidence, 0, 1, false)
  if (props.name === 'imageGeneration' && llmConfig.apiType === 'openai' && !llmConfig.editPath?.trim()) result[fieldId('editPath')] = '请输入编辑路径'
  if (llmConfig.baseUrl?.trim()) {
    try {
      const url = new URL(llmConfig.baseUrl)
      if (!['http:', 'https:'].includes(url.protocol) || !url.hostname) throw new Error()
    } catch { result[fieldId('baseUrl')] = '请输入有效的 HTTP 或 HTTPS 接口地址' }
  }
  return result
})

const saveLLMConfig = async (): Promise<void> => {
  if (saving.value || !validateForm()) return
  const snapshot = {...llmConfig}
  const name = props.name
  saving.value = true
  try {
    const config: Partial<LLMConfig> = {...snapshot, name}
    if (!supportsPromptCacheKey.value) delete config.promptCacheKeyEnabled
    if (!hasGenerationParameters.value) {
      delete config.maxTokens
      delete config.temperature
      delete config.topP
      delete config.reasoningEffort
    }
    const resp = await fetch('/admin/api/llm-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(config)
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast?.('模型配置已保存')
      savedConfig.value = JSON.stringify(snapshot)
      emit('saved', name, snapshot)
    } else {
      showToast?.(data.error || '保存失败', true)
    }
  } catch {
    showToast?.('保存失败，请检查网络连接', true)
  } finally {
    saving.value = false
  }
}

</script>

<template>
  <form class="model-config" :class="{'range-hints-dismissed': rangeHintsDismissed}" novalidate
        @focusin="rangeHintsDismissed = false" @keydown.esc="rangeHintsDismissed = true" @submit.prevent="saveLLMConfig">
    <h2 v-if="title" class="model-title">{{ title }}</h2>
    <p v-if="name === 'executorFallback'" class="fallback-hint">仅在默认模型请求失败时用于本次重试；留空不启用，下次请求仍优先使用默认模型。</p>
    <section class="config-section">
      <h3 class="section-title">接口与模型</h3>
      <div v-if="name === 'imageGeneration'" class="form-group">
        <label :for="fieldId('apiType')" class="form-label">接口类型</label>
        <select v-bind="fieldAttrs(fieldId('apiType'))" :disabled="saving" v-model="llmConfig.apiType" class="form-input">
          <option value="openai">OpenAI / CLIProxyAPI</option>
          <option value="openrouter">OpenRouter</option>
        </select>
        <FieldError :field="fieldId('apiType')" :message="errors[fieldId('apiType')]"/>
      </div>
      <div v-if="name === 'imageGeneration' && llmConfig.apiType === 'openai'" class="form-group">
        <label :for="fieldId('editPath')" class="form-label">编辑路径（Edit Path）</label>
        <input v-bind="fieldAttrs(fieldId('editPath'))" :disabled="saving" v-model="llmConfig.editPath" class="form-input" placeholder="/images/edits" type="text">
        <FieldError :field="fieldId('editPath')" :message="errors[fieldId('editPath')]"/>
      </div>
      <div class="form-group">
        <label :for="fieldId('apiKey')" class="form-label">API 密钥</label>
        <div class="secret-input">
          <input v-bind="fieldAttrs(fieldId('apiKey'))" :disabled="saving" v-model="llmConfig.apiKey" :type="showApiKey ? 'text' : 'password'" class="form-input" placeholder="sk-...">
          <button class="secret-toggle" type="button" :aria-controls="fieldId('apiKey')" :aria-pressed="showApiKey"
                  :aria-label="showApiKey ? '隐藏 API 密钥' : '显示 API 密钥'" :title="showApiKey ? '隐藏 API 密钥' : '显示 API 密钥'"
                  @click="showApiKey = !showApiKey"><NavIcon :name="showApiKey ? 'eye-off' : 'eye'"/></button>
        </div>
        <FieldError :field="fieldId('apiKey')" :message="errors[fieldId('apiKey')]"/>
      </div>
      <div class="form-group">
        <label :for="fieldId('baseUrl')" class="form-label">接口地址（Base URL）</label>
        <input v-bind="fieldAttrs(fieldId('baseUrl'))" :disabled="saving" v-model="llmConfig.baseUrl" class="form-input" placeholder="https://api.example.com" type="text">
        <FieldError :field="fieldId('baseUrl')" :message="errors[fieldId('baseUrl')]"/>
      </div>
      <div class="form-group">
        <label :for="fieldId('path')" class="form-label">{{ name === 'imageGeneration' ? '生成路径（Path）' : '请求路径（Path）' }}</label>
        <input v-bind="fieldAttrs(fieldId('path'))" :disabled="saving" v-model="llmConfig.path" class="form-input" placeholder="/v1/chat/completions" type="text">
        <FieldError :field="fieldId('path')" :message="errors[fieldId('path')]"/>
      </div>
      <div class="form-group">
        <label :for="fieldId('model')" class="form-label">模型名称</label>
        <input v-bind="fieldAttrs(fieldId('model'))" :disabled="saving" v-model="llmConfig.model" class="form-input" placeholder="gpt-4" type="text">
        <FieldError :field="fieldId('model')" :message="errors[fieldId('model')]"/>
      </div>
      <div v-for="field in requestFields" :key="field.key" class="form-group numeric-field">
        <div v-if="field.help" class="field-label">
          <label :for="fieldId(field.key)" class="form-label">{{ field.label }}</label>
          <FieldHelp :id="fieldId(field.key + '-details')" :label="field.label">{{ field.help }}</FieldHelp>
        </div>
        <label v-else :for="fieldId(field.key)" class="form-label">{{ field.label }}</label>
        <input v-bind="fieldAttrs(fieldId(field.key), fieldId(field.key) + '-hint')" :disabled="saving" v-model.number="llmConfig[field.key]" class="form-input" type="number" :min="field.minimum" :max="field.maximum" :step="field.step">
        <p :id="fieldId(field.key) + '-hint'" class="form-hint range-hint">{{ field.hint }}</p>
        <FieldError :field="fieldId(field.key)" :message="errors[fieldId(field.key)]"/>
      </div>
      <p v-if="name === 'imageGeneration'" class="important-note">生图超时不代表服务端未完成，开启重试可能重复生成并计费，默认不重试。</p>
    </section>
    <section v-if="hasParameters" class="config-section">
      <h3 class="section-title">{{ name === 'jev' ? '决策参数' : '生成参数' }}</h3>
      <div v-for="field in parameterFields" :key="field.key" class="form-group numeric-field">
        <div v-if="field.help" class="field-label">
          <label :for="fieldId(field.key)" class="form-label">{{ field.label }}</label>
          <FieldHelp :id="fieldId(field.key + '-details')" :label="field.label">{{ field.help }}</FieldHelp>
        </div>
        <label v-else :for="fieldId(field.key)" class="form-label">{{ field.label }}</label>
        <input v-bind="fieldAttrs(fieldId(field.key), fieldId(field.key) + '-hint')" :disabled="saving" v-model.number="llmConfig[field.key]" class="form-input" type="number" :min="field.minimum" :max="field.maximum" :step="field.step">
        <p :id="fieldId(field.key) + '-hint'" class="form-hint range-hint">{{ field.hint }}</p>
        <FieldError :field="fieldId(field.key)" :message="errors[fieldId(field.key)]"/>
      </div>
      <div v-if="supportsPromptCacheKey" class="form-group checkbox-field">
        <div class="checkbox-description">
          <div class="field-label">
            <label :for="fieldId('promptCacheKeyEnabled')" class="form-label">发送提示词缓存键</label>
            <FieldHelp :id="fieldId('promptCacheKeyEnabled-details')" label="发送提示词缓存键">默认关闭。开启后发送按会话生成的 prompt_cache_key，例如 insoulforge:executor:group:123456。</FieldHelp>
          </div>
          <p :id="fieldId('promptCacheKeyEnabled') + '-hint'" class="setting-summary">需接口支持，是否命中缓存由服务端决定。</p>
        </div>
        <input :id="fieldId('promptCacheKeyEnabled')" v-model="llmConfig.promptCacheKeyEnabled" type="checkbox" :disabled="saving" :aria-describedby="fieldId('promptCacheKeyEnabled') + '-hint'">
      </div>
      <div v-if="hasGenerationParameters" class="form-group">
        <div class="field-label">
          <label :for="fieldId('reasoningEffort')" class="form-label">思考强度（Reasoning Effort）</label>
          <FieldHelp :id="fieldId('reasoningEffort') + '-hint'" label="思考强度">各模型支持的选项不同，部分模型不支持关闭思考；不发送则使用服务端默认值。</FieldHelp>
        </div>
        <select v-bind="fieldAttrs(fieldId('reasoningEffort'), fieldId('reasoningEffort') + '-hint')" :disabled="saving" v-model="llmConfig.reasoningEffort" class="form-input">
          <option value="">不发送（默认）</option>
          <option value="none">关闭思考 (none)</option>
          <option value="minimal">极低强度 (minimal)</option>
          <option value="low">低强度 (low)</option>
          <option value="medium">中等思考 (medium)</option>
          <option value="high">深度思考 (high)</option>
          <option value="xhigh">极高强度 (xhigh)</option>
          <option value="max">最大强度 (max)</option>
        </select>
        <FieldError :field="fieldId('reasoningEffort')" :message="errors[fieldId('reasoningEffort')]"/>
      </div>
    </section>
    <div class="save-actions">
      <button :disabled="saving" class="btn btn-primary" type="submit">{{ saving ? '保存中...' : '保存配置' }}</button>
      <p class="save-hint" :class="{'has-changes': dirty}" role="status">{{ dirty ? '有未保存的修改' : '修改后需保存' }}</p>
    </div>
  </form>
</template>

<style scoped>
.model-config { display: grid; gap: 16px; max-width: 1120px; margin-bottom: 24px; }
.model-title { margin: 0; font-size: 16px; font-weight: 600; color: var(--text-secondary); }
.config-section { min-width: 0; padding: 20px 24px; background: var(--card-bg); border-radius: var(--radius-md); }
.section-title { margin-bottom: 12px; font-size: 16px; font-weight: 600; color: var(--text-secondary); }
.config-section .form-group {
  position: relative;
  display: grid;
  grid-template-columns: minmax(0, 220px) minmax(0, 440px) minmax(0, 1fr);
  align-items: center;
  gap: 8px 16px;
  min-width: 0;
  margin: 0;
  padding: 12px 0;
}
.config-section .form-group:first-of-type { padding-top: 0; }
.config-section .form-group:last-child { padding-bottom: 0; }
.config-section .form-group + .form-group::before {
  content: '';
  position: absolute;
  top: 0;
  inset-inline-start: 0;
  width: min(676px, 100%);
  border-top: 1px solid color-mix(in srgb, var(--border) 35%, transparent);
  pointer-events: none;
}
.config-section .numeric-field { grid-template-columns: minmax(0, 220px) minmax(0, 160px) minmax(0, 1fr); }
.config-section .numeric-field::before { width: min(396px, 100%); }
.field-label { display: flex; align-items: center; gap: 8px; min-width: 0; }
.form-label { margin: 0; }
.form-input {
  height: 36px;
  min-height: 36px;
  padding: 6px 10px;
  line-height: 22px;
  background-color: color-mix(in srgb, var(--input-bg) 75%, var(--card-bg));
}
.range-hint { margin: 0; color: var(--text-secondary); visibility: hidden; }
.model-config:not(.range-hints-dismissed) .numeric-field > .form-input:focus ~ .range-hint { visibility: visible; }
.form-group :deep(.form-error) { grid-column: 2 / -1; margin: 0; }
.config-section .checkbox-field { display: flex; align-items: center; justify-content: space-between; gap: 16px; }
.checkbox-description { min-width: 0; }
.checkbox-field .form-label { cursor: pointer; }
.checkbox-field > input { flex-shrink: 0; }
.fallback-hint, .setting-summary, .save-hint, .important-note { margin: 0; font-size: 12px; color: var(--text-secondary); }
.setting-summary { margin-top: 4px; }
.important-note { color: var(--warning); }
.save-actions { display: flex; align-items: center; flex-wrap: wrap; gap: 8px 12px; }
.save-hint.has-changes { color: var(--warning); font-weight: 500; }
@media (max-width: 600px) {
  .config-section .form-group { grid-template-columns: minmax(0, 1fr); }
  .config-section .numeric-field { grid-template-columns: minmax(0, 1fr) minmax(0, 120px); }
  .range-hint { display: none; }
  .model-config:not(.range-hints-dismissed) .numeric-field > .form-input:focus ~ .range-hint { display: block; grid-column: 1 / -1; }
  .form-group :deep(.form-error) { grid-column: 1 / -1; }
}
</style>
