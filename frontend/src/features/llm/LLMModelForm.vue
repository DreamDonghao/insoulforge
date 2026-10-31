<script lang="ts" setup>
import {computed, inject, reactive, ref, type Ref, watch} from 'vue'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {I32_MAX, numberError} from '../../utils/validation'
import {useUnsavedChanges} from '../../composables/useUnsavedChanges'
import type {ApiResponse, LLMConfig} from '../../vite-env'

const props = defineProps<{name: string, initialConfig: LLMConfig, title?: string}>()
const hasParameters = computed(() => !['embedding', 'imageGeneration'].includes(props.name))
const hasGenerationParameters = computed(() => hasParameters.value && props.name !== 'jev')
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
watch(() => props.initialConfig, config => {
  const preserveDraft = dirty.value || saving.value
  savedConfig.value = JSON.stringify(config)
  if (!preserveDraft) Object.assign(llmConfig, config)
})
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
  <form class="model-config" novalidate @submit.prevent="saveLLMConfig">
    <h2 v-if="title" class="model-title">{{ title }}</h2>
    <p v-if="name === 'executorFallback'" class="fallback-hint">仅在默认模型请求失败时用于本次重试；留空不启用，下次请求仍优先使用默认模型。</p>
    <div class="config-section">
      <h3 class="section-title">接口与模型</h3>
      <div v-if="name === 'imageGeneration'" class="form-row">
        <div class="form-group">
          <label :for="fieldId('apiType')" class="form-label">接口类型</label>
          <select v-bind="fieldAttrs(fieldId('apiType'))" :disabled="saving" v-model="llmConfig.apiType" class="form-input">
            <option value="openai">OpenAI / CLIProxyAPI</option>
            <option value="openrouter">OpenRouter</option>
          </select>
          <FieldError :field="fieldId('apiType')" :message="errors[fieldId('apiType')]"/>
        </div>
        <div v-if="llmConfig.apiType === 'openai'" class="form-group">
          <label :for="fieldId('editPath')" class="form-label">编辑路径（Edit Path）</label>
          <input v-bind="fieldAttrs(fieldId('editPath'))" :disabled="saving" v-model="llmConfig.editPath" class="form-input" placeholder="/images/edits" type="text">
          <FieldError :field="fieldId('editPath')" :message="errors[fieldId('editPath')]"/>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label :for="fieldId('apiKey')" class="form-label">API 密钥</label>
          <div class="api-key-row">
            <input v-bind="fieldAttrs(fieldId('apiKey'))" :disabled="saving" v-model="llmConfig.apiKey" :type="showApiKey ? 'text' : 'password'" class="form-input"
                   placeholder="sk-...">
          <FieldError :field="fieldId('apiKey')" :message="errors[fieldId('apiKey')]"/>
            <button class="btn btn-secondary btn-sm api-key-toggle" type="button" @click="showApiKey = !showApiKey">
              {{ showApiKey ? '隐藏' : '显示' }}
            </button>
          </div>
        </div>
        <div class="form-group">
          <label :for="fieldId('baseUrl')" class="form-label">接口地址（Base URL）</label>
          <input v-bind="fieldAttrs(fieldId('baseUrl'))" :disabled="saving" v-model="llmConfig.baseUrl" class="form-input" placeholder="https://api.example.com" type="text">
          <FieldError :field="fieldId('baseUrl')" :message="errors[fieldId('baseUrl')]"/>
        </div>
      </div>
      <div class="form-row">
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
      </div>
      <div class="form-row">
        <div class="form-group">
          <label :for="fieldId('timeoutSeconds')" class="form-label">请求超时（秒）</label>
          <input v-bind="fieldAttrs(fieldId('timeoutSeconds'), fieldId('timeoutSeconds') + '-hint')" :disabled="saving" v-model.number="llmConfig.timeoutSeconds" class="form-input" type="number" min="1" max="3600" step="1">
          <FieldError :field="fieldId('timeoutSeconds')" :message="errors[fieldId('timeoutSeconds')]"/>
          <p :id="fieldId('timeoutSeconds') + '-hint'" class="form-hint">1～3600 秒，整数。每次尝试独立计时，保存后对后续请求生效。</p>
        </div>
        <div class="form-group">
          <label :for="fieldId('maxRetries')" class="form-label">失败重试次数</label>
          <input v-bind="fieldAttrs(fieldId('maxRetries'), fieldId('maxRetries') + '-hint')" :disabled="saving" v-model.number="llmConfig.maxRetries" class="form-input" type="number" min="0" max="10" step="1">
          <FieldError :field="fieldId('maxRetries')" :message="errors[fieldId('maxRetries')]"/>
          <p :id="fieldId('maxRetries') + '-hint'" class="form-hint">0～10 次，整数。不包含首次请求；0 表示不重试，每次重试间隔 1 秒。</p>
        </div>
      </div>
      <p v-if="name === 'imageGeneration'" class="form-hint">生图超时不代表服务端未完成，开启重试可能重复生成并计费，默认不重试。</p>
    </div>
    <div v-if="hasParameters" class="config-section">
      <h3 class="section-title">{{ name === 'jev' ? '决策参数' : '生成参数' }}</h3>
      <div v-if="hasGenerationParameters" class="form-row sampling-row">
        <div class="form-group">
          <label :for="fieldId('maxTokens')" class="form-label">最大输出 Token</label>
          <input v-bind="fieldAttrs(fieldId('maxTokens'), fieldId('maxTokens') + '-hint')" :disabled="saving" v-model.number="llmConfig.maxTokens" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError :field="fieldId('maxTokens')" :message="errors[fieldId('maxTokens')]"/>
          <p :id="fieldId('maxTokens') + '-hint'" class="form-hint">1～2147483647 Token，整数；实际输出上限受模型支持范围限制。</p>
        </div>
        <div class="form-group">
          <label :for="fieldId('temperature')" class="form-label">温度（Temperature）</label>
          <input v-bind="fieldAttrs(fieldId('temperature'), fieldId('temperature') + '-hint')" :disabled="saving" v-model.number="llmConfig.temperature" class="form-input" max="2" min="0" step="any" type="number">
          <FieldError :field="fieldId('temperature')" :message="errors[fieldId('temperature')]"/>
          <p :id="fieldId('temperature') + '-hint'" class="form-hint">0～2，允许小数；具体支持范围以模型为准。</p>
        </div>
        <div class="form-group">
          <label :for="fieldId('topP')" class="form-label">采样范围（Top P）</label>
          <input v-bind="fieldAttrs(fieldId('topP'), fieldId('topP') + '-hint')" :disabled="saving" v-model.number="llmConfig.topP" class="form-input" max="1" min="0" step="any" type="number">
          <FieldError :field="fieldId('topP')" :message="errors[fieldId('topP')]"/>
          <p :id="fieldId('topP') + '-hint'" class="form-hint">0～1，允许小数。</p>
        </div>
      </div>
      <div v-if="name === 'jev'" class="form-row">
        <div class="form-group">
          <label :for="fieldId('minConfidence')" class="form-label">最低置信度</label>
          <input v-bind="fieldAttrs(fieldId('minConfidence'), fieldId('minConfidence') + '-hint')" :disabled="saving" v-model.number="llmConfig.minConfidence" class="form-input" type="number" min="0" max="1" step="any">
          <FieldError :field="fieldId('minConfidence')" :message="errors[fieldId('minConfidence')]"/>
          <p :id="fieldId('minConfidence') + '-hint'" class="form-hint">0～1，允许小数。低于此值时由 Router LLM 判断，默认 0.60</p>
        </div>
      </div>
      <div v-if="hasGenerationParameters" class="form-row">
        <div class="form-group">
          <label :for="fieldId('reasoningEffort')" class="form-label">思考强度（Reasoning Effort）</label>
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
          <p :id="fieldId('reasoningEffort') + '-hint'" class="form-hint">各模型支持的选项不同，部分模型不支持关闭思考；不发送则使用服务端默认值。</p>
        </div>
      </div>
    </div>
    <button :disabled="saving" class="btn btn-primary" type="submit">
      {{ saving ? '保存中...' : '保存配置' }}
    </button>
  </form>
</template>

<style scoped>
.model-config {
  padding: 20px 0;
  max-width: 1120px;
}

.model-config + .model-config {
  border-top: 1px solid var(--border-color);
}

.model-title {
  margin: 0 0 16px;
  padding-left: 10px;
  border-left: 3px solid var(--primary);
  font-size: 16px;
}

.config-section {
  padding-block: 16px;
  border-top: 1px solid var(--border);
}

.section-title {
  margin-bottom: 14px;
  font-size: 13px;
  font-weight: 600;
  color: var(--text-secondary);
}

.config-section .form-row {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.config-section .form-group {
  min-width: 0;
  margin-bottom: 12px;
}

.config-section .sampling-row {
  grid-template-columns: repeat(3, minmax(0, 1fr));
}

.fallback-hint {
  margin-bottom: 16px;
  color: var(--text-secondary);
  font-size: 12px;
}

@media (max-width: 800px) {
  .config-section .form-row {
    grid-template-columns: 1fr;
  }
}

.api-key-row {
  display: flex;
  gap: 8px;
  align-items: center;
}

.api-key-row .form-input {
  flex: 1;
  min-width: 0;
}

.api-key-toggle {
  align-self: stretch;
  flex-shrink: 0;
  min-width: 64px;
}
</style>
