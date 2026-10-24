<script lang="ts" setup>
import {computed, inject, reactive, ref, type Ref, watch} from 'vue'
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
const showApiKey: Ref<boolean> = ref(false)
watch(() => props.initialConfig, config => Object.assign(llmConfig, config))
const saveLLMConfig = async (): Promise<void> => {
  saving.value = true
  try {
    const config = {...llmConfig, name: props.name}
    const resp = await fetch('/admin/api/llm-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(config)
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast?.('模型配置已保存')
      emit('saved', props.name, config)
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
  <section class="model-config">
    <h2 v-if="title" class="model-title">{{ title }}</h2>
    <p v-if="name === 'executorFallback'" class="fallback-hint">仅在默认模型请求失败时用于本次重试；留空不启用，下次请求仍优先使用默认模型。</p>
    <div class="config-section">
      <h3 class="section-title">接口与模型</h3>
      <div v-if="name === 'imageGeneration'" class="form-row">
        <div class="form-group">
          <label class="form-label">接口类型</label>
          <select v-model="llmConfig.apiType" class="form-input">
            <option value="openai">OpenAI / CLIProxyAPI</option>
            <option value="openrouter">OpenRouter</option>
          </select>
        </div>
        <div v-if="llmConfig.apiType === 'openai'" class="form-group">
          <label class="form-label">编辑路径（Edit Path）</label>
          <input v-model="llmConfig.editPath" class="form-input" placeholder="/images/edits" type="text">
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">API 密钥</label>
          <div class="api-key-row">
            <input v-model="llmConfig.apiKey" :type="showApiKey ? 'text' : 'password'" class="form-input"
                   placeholder="sk-...">
            <button class="btn btn-secondary btn-sm api-key-toggle" type="button" @click="showApiKey = !showApiKey">
              {{ showApiKey ? '隐藏' : '显示' }}
            </button>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">接口地址（Base URL）</label>
          <input v-model="llmConfig.baseUrl" class="form-input" placeholder="https://api.example.com" type="text">
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">{{ name === 'imageGeneration' ? '生成路径（Path）' : '请求路径（Path）' }}</label>
          <input v-model="llmConfig.path" class="form-input" placeholder="/v1/chat/completions" type="text">
        </div>
        <div class="form-group">
          <label class="form-label">模型名称</label>
          <input v-model="llmConfig.model" class="form-input" placeholder="gpt-4" type="text">
        </div>
      </div>
    </div>
    <div v-if="hasParameters" class="config-section">
      <h3 class="section-title">{{ name === 'jev' ? '决策参数' : '生成参数' }}</h3>
      <div v-if="hasGenerationParameters" class="form-row sampling-row">
        <div class="form-group">
          <label class="form-label">最大输出 Token</label>
          <input v-model.number="llmConfig.maxTokens" class="form-input" type="number">
        </div>
        <div class="form-group">
          <label class="form-label">温度（Temperature）</label>
          <input v-model.number="llmConfig.temperature" class="form-input" max="2" min="0" step="0.1" type="number">
        </div>
        <div class="form-group">
          <label class="form-label">采样范围（Top P）</label>
          <input v-model.number="llmConfig.topP" class="form-input" max="1" min="0" step="0.1" type="number">
        </div>
      </div>
      <div v-if="name === 'jev'" class="form-row">
        <div class="form-group">
          <label class="form-label">最低置信度</label>
          <input v-model.number="llmConfig.minConfidence" class="form-input" type="number" min="0" max="1" step="0.05">
          <p class="form-hint">低于此值时由 Router LLM 判断，默认 0.60</p>
        </div>
      </div>
      <div v-if="hasGenerationParameters" class="form-row">
        <div class="form-group">
          <label class="form-label">思考强度（Reasoning Effort）</label>
          <select v-model="llmConfig.reasoningEffort" class="form-input">
            <option value="">不发送（默认）</option>
            <option value="none">关闭思考 (none)</option>
            <option value="minimal">极低强度 (minimal)</option>
            <option value="low">低强度 (low)</option>
            <option value="medium">中等思考 (medium)</option>
            <option value="high">深度思考 (high)</option>
            <option value="xhigh">极高强度 (xhigh)</option>
            <option value="max">最大强度 (max)</option>
          </select>
          <p class="form-hint">各模型支持的选项不同，部分模型不支持关闭思考；不发送则使用服务端默认值。</p>
        </div>
      </div>
    </div>
    <button :disabled="saving" class="btn btn-primary" @click="saveLLMConfig">
      {{ saving ? '保存中...' : '保存配置' }}
    </button>
  </section>
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
