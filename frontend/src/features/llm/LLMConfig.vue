<script lang="ts" setup>
/**
 * @file LLMConfig.vue
 * @brief LLM 配置管理组件
 */
import {inject, reactive, ref, type Ref, onMounted} from 'vue'
import type {LLMConfig} from '../../vite-env'

import LLMModelForm from './LLMModelForm.vue'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const llmNames = ['router', 'jev', 'executor', 'executorThinking', 'image', 'imageGeneration', 'memory', 'embedding']
const llmLabels: Record<string, string> = {
  router: 'Router',
  jev: 'Jev',
  executor: 'Executor',
  executorFallback: 'Executor 兜底',
  executorThinking: 'Executor思考',
  image: 'Image',
  imageGeneration: '图片生成',
  memory: 'Memory',
  embedding: 'Embedding'
}
// 各配置的实际用途（与后端代码一致）
const llmUsages: Record<string, string> = {
  router: '用途：Jev 未配置、结果不确定或请求失败时的回复决策',
  jev: '用途：群聊的优先回复决策；低置信度时由 Router 再判断',
  executor: '用途：回复生成',
  executorFallback: '用途：默认 Executor 请求失败时重试本次请求；留空不启用，下次仍优先使用默认模型',
  executorThinking: '用途：深度思考模式的分析阶段（最终执行仍走 Executor）',
  image: '用途：图片识别与描述',
  imageGeneration: '用途：异步文生图，生成完成后直接发送到会话',
  memory: '用途：记忆提取、整理与好感度评分',
  embedding: '用途：长期记忆向量化（记忆写入与检索时计算文本向量）'
}
const selectedLLM: Ref<string> = ref('router')
const llmConfigs = reactive<Record<string, LLMConfig>>({})
onMounted(async () => {
  try {
    const resp = await fetch('/admin/api/llm-configs')
    if (!resp.ok) throw new Error('加载失败')
    Object.assign(llmConfigs, await resp.json())
  } catch {
    showToast?.('模型配置加载失败', true)
  }
})

const updateConfig = (name: string, config: LLMConfig): void => {
  llmConfigs[name] = config
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

    <template v-for="name in (selectedLLM === 'executor' ? ['executor', 'executorFallback'] : [selectedLLM])" :key="name">
      <LLMModelForm
          v-if="llmConfigs[name]"
          :name="name"
          :initial-config="llmConfigs[name]"
          :title="selectedLLM === 'executor' ? (name === 'executor' ? '默认模型' : '兜底模型（可选）') : undefined"
          @saved="updateConfig"
      />
    </template>
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

</style>
