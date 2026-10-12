<script lang="ts" setup>
import {inject, reactive, ref, type Ref, watch} from 'vue'
import type {ApiResponse, LLMConfig} from '../../vite-env'

const props = defineProps<{name: string, initialConfig: LLMConfig, title?: string}>()
const emit = defineEmits<{saved: [name: string, config: LLMConfig]}>()
const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const llmConfig = reactive<LLMConfig>({...props.initialConfig})
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
      showToast?.('LLM配置已保存')
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
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">API Key</label>
          <div class="api-key-row">
            <input v-model="llmConfig.apiKey" :type="showApiKey ? 'text' : 'password'" class="form-input"
                   placeholder="sk-...">
            <button class="btn btn-secondary btn-sm api-key-toggle" type="button" @click="showApiKey = !showApiKey">
              {{ showApiKey ? '● 隐藏' : '○ 显示' }}
            </button>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">Base URL</label>
          <input v-model="llmConfig.baseUrl" class="form-input" placeholder="https://api.example.com" type="text">
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">Path</label>
          <input v-model="llmConfig.path" class="form-input" placeholder="/v1/chat/completions" type="text">
        </div>
        <div class="form-group">
          <label class="form-label">Model</label>
          <input v-model="llmConfig.model" class="form-input" placeholder="gpt-4" type="text">
        </div>
      </div>
      <div v-if="name !== 'embedding' && name !== 'jev' && name !== 'imageGeneration'" class="form-row">
        <div class="form-group">
          <label class="form-label">Max Tokens</label>
          <input v-model.number="llmConfig.maxTokens" class="form-input" type="number">
        </div>
        <div class="form-group">
          <label class="form-label">Temperature</label>
          <input v-model.number="llmConfig.temperature" class="form-input" max="2" min="0" step="0.1" type="number">
        </div>
        <div class="form-group">
          <label class="form-label">Top P</label>
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
      <div v-if="name !== 'embedding' && name !== 'jev' && name !== 'imageGeneration'" class="form-row">
        <div class="form-group">
          <label class="form-label">Reasoning Effort（Gemma等模型专用）</label>
          <select v-model="llmConfig.reasoningEffort" class="form-input">
            <option value="">不发送（默认）</option>
            <option value="none">关闭思考 (none)</option>
            <option value="medium">中等思考 (medium)</option>
            <option value="high">深度思考 (high)</option>
          </select>
          <p class="form-hint">设置为"关闭思考"可大幅减少token消耗，适合简单任务（如Router）</p>
        </div>
      </div>
      <button :disabled="saving" class="btn btn-primary" @click="saveLLMConfig">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </section>
</template>

<style scoped>
.model-config {
  padding: 24px 0;
}

.model-config + .model-config {
  border-top: 1px solid var(--border-color);
}

.model-title {
  margin: 0 0 20px;
  font-size: 18px;
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
  flex-shrink: 0;
  min-width: 64px;
}
</style>
