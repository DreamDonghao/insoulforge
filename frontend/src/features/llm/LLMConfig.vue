<script lang="ts" setup>
/**
 * @file LLMConfig.vue
 * @brief LLM 配置管理组件
 */
import {computed, inject, reactive, ref, type Ref, onMounted} from 'vue'
import type {LLMConfig} from '../../vite-env'

import {confirmDiscard} from '../../composables/useUnsavedChanges'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import LLMModelForm from './LLMModelForm.vue'
import SlidingTabs from '../../components/SlidingTabs.vue'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const llmNames = ['router', 'executor', 'executorThinking', 'image', 'asr', 'imageGeneration', 'memory', 'embedding']
const llmLabels: Record<string, string> = {
  router: '回复决策',
  executor: '回复生成',
  executorThinking: '深度思考',
  image: '图片识别',
  asr: '语音识别',
  imageGeneration: '图片生成',
  memory: '记忆维护',
  embedding: '向量化'
}
const modelTabs = llmNames.map(key => ({key, label: llmLabels[key] || key}))
const modelGroups: Record<string, {key: string, label: string}[]> = {
  router: [{key: 'router', label: 'Router LLM'}, {key: 'jev', label: 'Jev（可选）'}],
  executor: [{key: 'executor', label: '默认模型'}, {key: 'executorFallback', label: '备用模型（可选）'}]
}

// 各配置的实际用途（与后端代码一致）
const llmUsages: Record<string, string> = {
  router: '群聊优先使用 Jev。Jev 未配置、判断不明确、置信度不足或请求失败时，由 Router LLM 判断。',
  executor: '默认模型请求失败时可用备用模型，留空则关闭备用。',
  executorThinking: '用途：深度思考模式的分析阶段（最终执行仍走 Executor）',
  image: '用途：图片识别与描述',
  asr: '自动转写已启用会话的语音。填写 API 密钥后启用，音频将发送到配置的语音识别服务。',
  imageGeneration: '用途：异步文生图，生成完成后直接发送到会话',
  memory: '用途：人物记忆提取、会话记忆与人物档案整理',
  embedding: '用途：长期记忆向量化（记忆写入与检索时计算文本向量）'
}
const selectedLLM: Ref<string> = ref('router')
const activeModel = ref('router')
const modelVariants = computed(() => modelGroups[selectedLLM.value])
const modelForm = ref<InstanceType<typeof LLMModelForm> | null>(null)
const canSwitchModel = (): boolean => {
  if (modelForm.value?.saving) {
    showToast?.('正在保存，请等待完成后再切换', true)
    return false
  }
  return confirmDiscard(modelForm.value?.dirty ?? false)
}
const selectModel = (name: string): void => {
  if (name === selectedLLM.value || !canSwitchModel()) return
  selectedLLM.value = name
  activeModel.value = name
}
const selectVariant = (name: string): void => {
  if (name !== activeModel.value && canSwitchModel()) activeModel.value = name
}
const changeVariant = (event: Event): void => {
  const select = event.currentTarget as HTMLSelectElement
  selectVariant(select.value)
  select.value = activeModel.value
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
        <div :key="selectedLLM" class="model-panel" :class="{'has-model-selector': modelVariants}">
          <header v-if="modelVariants" class="model-panel-header">
            <div>
              <h2>{{ llmLabels[selectedLLM] }}</h2>
              <p class="form-hint">{{ llmUsages[selectedLLM] }}</p>
            </div>
            <select class="form-input model-selector" :aria-label="llmLabels[selectedLLM] + '模型'"
                    :value="activeModel" :disabled="modelForm?.saving" @change="changeVariant">
              <option v-for="variant in modelVariants" :key="variant.key" :value="variant.key">{{ variant.label }}</option>
            </select>
          </header>
          <p v-else class="form-hint">{{ llmUsages[selectedLLM] }}</p>
          <LLMModelForm v-if="llmConfigs[activeModel]" :key="activeModel" ref="modelForm"
                        :name="activeModel" :initial-config="llmConfigs[activeModel]" @saved="updateConfig"/>
        </div>
      </template>
    </SlidingTabs>
  </div>
</template>

<style scoped>
.model-panel {
  animation: model-panel-enter 160ms ease-out;
}

.model-panel-header {
  display: flex;
  align-items: start;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px 24px;
  max-width: 1120px;
  padding: 20px 24px;
  background: var(--card-bg);
  border-radius: var(--radius-md) var(--radius-md) 0 0;
  border-bottom: 1px solid var(--border);
}

.model-panel-header > div {
  flex: 1 1 280px;
}

.model-panel-header h2 {
  margin: 0 0 4px;
  font-size: 16px;
}

.model-panel-header .form-hint {
  margin: 0;
}

.model-selector {
  width: auto;
  max-width: 100%;
  min-height: 36px;
}

.has-model-selector :deep(.config-section:first-child) {
  border-start-start-radius: 0;
  border-start-end-radius: 0;
}

@media (max-width: 760px) {
  .model-selector {
    min-height: 44px;
    font-size: 16px;
  }
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
