<script lang="ts" setup>
/**
 * @file LLMConfig.vue
 * @brief LLM 配置管理组件
 */
import {inject, reactive, ref, type Ref, watch} from 'vue'
import type {ApiResponse, LLMConfig} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const llmNames = ['router', 'jev', 'executor', 'executorThinking', 'image', 'memory', 'embedding']
const llmLabels: Record<string, string> = {
  router: 'Router',
  jev: 'Jev',
  executor: 'Executor',
  executorThinking: 'Executor思考',
  image: 'Image',
  memory: 'Memory',
  embedding: 'Embedding'
}
// 各配置的实际用途（与后端代码一致）
const llmUsages: Record<string, string> = {
  router: '用途：Jev 未配置、结果不确定或请求失败时的回复决策',
  jev: '用途：群聊的优先回复决策；低置信度时由 Router 再判断',
  executor: '用途：回复生成',
  executorThinking: '用途：深度思考模式的分析阶段（最终执行仍走 Executor）',
  image: '用途：图片识别与描述',
  memory: '用途：记忆提取、整理与好感度评分',
  embedding: '用途：长期记忆向量化（记忆写入与检索时计算文本向量）'
}
const selectedLLM: Ref<string> = ref('router')
const llmConfigs = reactive<Record<string, LLMConfig>>({})
const llmConfig = reactive<LLMConfig>({
  apiKey: '', baseUrl: '', path: '', model: '',
  maxTokens: 100, temperature: 0.7, topP: 0.9, reasoningEffort: '', minConfidence: 0.6
})
const saving: Ref<boolean> = ref(false)
const showApiKey: Ref<boolean> = ref(false)

// 首次加载所有配置
watch(selectedLLM, async (name: string) => {
  if (Object.keys(llmConfigs).length === 0) {
    const resp = await fetch('/admin/api/llm-configs')
    const data = await resp.json()
    Object.assign(llmConfigs, data)
  }
  if (llmConfigs[name]) {
    Object.assign(llmConfig, llmConfigs[name])
  }
}, {immediate: true})

const saveLLMConfig = async (): Promise<void> => {
  saving.value = true
  try {
    llmConfig.name = selectedLLM.value
    const resp = await fetch('/admin/api/llm-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(llmConfig)
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('LLM配置已保存')
      llmConfigs[selectedLLM.value] = {...llmConfig}
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } finally {
    saving.value = false
  }
}
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">LLM配置</h1>
      <p class="page-subtitle">配置各Agent的模型参数</p>
    </div>

    <div class="tabs">
      <button
          v-for="name in llmNames"
          :key="name"
          :class="{ active: selectedLLM === name }"
          class="tab"
          @click="selectedLLM = name"
      >{{ llmLabels[name] || name }}
      </button>
    </div>

    <p class="form-hint">{{ llmUsages[selectedLLM] }}</p>

    <div class="card">
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
      <div v-if="selectedLLM !== 'embedding' && selectedLLM !== 'jev'" class="form-row">
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
      <div v-if="selectedLLM === 'jev'" class="form-row">
        <div class="form-group">
          <label class="form-label">最低置信度</label>
          <input v-model.number="llmConfig.minConfidence" class="form-input" type="number" min="0" max="1" step="0.05">
          <p class="form-hint">低于此值时由 Router LLM 判断，默认 0.60</p>
        </div>
      </div>
      <div v-if="selectedLLM !== 'embedding' && selectedLLM !== 'jev'" class="form-row">
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
    </div>
  </div>
</template>

<style scoped>
.tabs {
  max-width: 100%;
  overflow-x: auto;
}

.tab {
  flex: none;
}

.api-key-row {
  display: flex;
  gap: 8px;
  align-items: center;
}

.api-key-row .form-input {
  flex: 1;
}

.api-key-toggle {
  flex-shrink: 0;
  min-width: 64px;
}
</style>
