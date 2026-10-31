<script lang="ts" setup>
/**
 * @file LLMConfig.vue
 * @brief LLM 配置管理组件
 */
import {inject, reactive, ref, type Ref, onMounted} from 'vue'
import type {LLMConfig} from '../../vite-env'

import {confirmDiscard} from '../../composables/useUnsavedChanges'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import LLMModelForm from './LLMModelForm.vue'
import SlidingTabs from '../../components/SlidingTabs.vue'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const llmNames = ['router', 'jev', 'executor', 'executorThinking', 'image', 'imageGeneration', 'memory', 'embedding']
const llmLabels: Record<string, string> = {
  router: '回复决策',
  jev: 'Jev',
  executor: '回复生成',
  executorFallback: '备用模型',
  executorThinking: '深度思考',
  image: '图片识别',
  imageGeneration: '图片生成',
  memory: '记忆维护',
  embedding: '向量化'
}
const modelTabs = llmNames.map(key => ({key, label: llmLabels[key] || key}))

// 各配置的实际用途（与后端代码一致）
const llmUsages: Record<string, string> = {
  router: '用途：Jev 未配置、结果不确定或请求失败时的回复决策',
  jev: '用途：群聊的优先回复决策；低置信度时由 Router 再判断',
  executor: '用途：回复生成',
  executorFallback: '用途：默认 Executor 请求失败时重试本次请求；留空不启用，下次仍优先使用默认模型',
  executorThinking: '用途：深度思考模式的分析阶段（最终执行仍走 Executor）',
  image: '用途：图片识别与描述',
  imageGeneration: '用途：异步文生图，生成完成后直接发送到会话',
  memory: '用途：人物记忆提取、会话记忆与人物档案整理',
  embedding: '用途：长期记忆向量化（记忆写入与检索时计算文本向量）'
}
const selectedLLM: Ref<string> = ref('router')
const modelForms = ref<InstanceType<typeof LLMModelForm>[]>([])
const selectModel = (name: string): void => {
  if (name === selectedLLM.value) return
  if (modelForms.value.some(form => form.saving)) {
    showToast?.('正在保存，请等待完成后再切换', true)
    return
  }
  if (confirmDiscard(modelForms.value.some(form => form.dirty))) selectedLLM.value = name
}
const llmConfigs = reactive<Record<string, LLMConfig>>({})
const loading = ref(false)
const loadError = ref('')
const loadConfigs = async (): Promise<void> => {
  if (loading.value) return
  loading.value = true
  try {
    const data = await requestJson<Record<string, LLMConfig>>('/admin/api/llm-configs')
    if (!data || Array.isArray(data) || typeof data !== 'object' || !data.router) throw new Error('模型配置数据无效')
    Object.assign(llmConfigs, data)
    loadError.value = ''
  } catch (cause) {
    loadError.value = requestError(cause, '模型配置加载失败')
  } finally {
    loading.value = false
  }
}
onMounted(loadConfigs)

const updateConfig = (name: string, config: LLMConfig): void => {
  llmConfigs[name] = config
}
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">模型配置</h1>
      <p class="page-subtitle">配置各功能使用的模型与请求参数</p>
    </div>

    <RequestError :message="loadError" :busy="loading" @retry="loadConfigs"/>
    <p v-if="loading" role="status">加载中…</p>
    <SlidingTabs v-if="!loadError && !loading" :model-value="selectedLLM" @update:model-value="selectModel" :items="modelTabs" label="模型功能" id-prefix="model-config">
      <template #panel>
        <div :key="selectedLLM" class="model-panel">
          <p class="form-hint">{{ llmUsages[selectedLLM] }}</p>

          <template v-for="name in (selectedLLM === 'executor' ? ['executor', 'executorFallback'] : [selectedLLM])" :key="name">
            <LLMModelForm
                v-if="llmConfigs[name]"
                ref="modelForms"
                :name="name"
                :initial-config="llmConfigs[name]"
                :title="selectedLLM === 'executor' ? (name === 'executor' ? '默认模型' : '备用模型（可选）') : undefined"
                @saved="updateConfig"
            />
          </template>
        </div>
      </template>
    </SlidingTabs>
  </div>
</template>

<style scoped>
.model-panel {
  animation: model-panel-enter 160ms ease-out;
}

@keyframes model-panel-enter {
  from {
    opacity: 0;
    transform: translateY(6px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

@media (prefers-reduced-motion: reduce) {
  .model-panel {
    animation: none;
  }
}

</style>
