<script lang="ts" setup>
/**
 * @file MemoryConfig.vue
 * @brief 记忆配置组件
 */
import {inject, onMounted, reactive, ref, type Ref} from 'vue'
import type {ApiResponse, MemoryConfig} from '../../vite-env.d'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {I32_MAX, numberError} from '../../utils/validation'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const memoryConfig = reactive<MemoryConfig>({
  affinityEnabled: true,
  contextWindowLimit: 100,
  sharedConversationMessageCount: 10,
  memorySummaryTriggerCount: 100,
  memorySummaryBatchSize: 50,
  memorySummaryContextCount: 10,
  conversationSummaryMaxChars: 100,
  conversationSummaryMaxCount: 10,
  personMemoryEntryMaxChars: 30,
  longTermMemoryEntryMaxChars: 100,
  personProfileEntryMaxChars: 50,
  personProfileMaxChars: 300,
  personRelationshipEntryMaxChars: 60,
  personRelationshipMaxChars: 150,
  personInteractionEntryMaxChars: 60,
  personInteractionMaxChars: 250,
  jevWindowSize: 10,
  routerWindowTriggerCount: 20,
  routerWindowKeepCount: 10,
  longTermRecallThreshold: 0.65,
  longTermInjectThreshold: 0.45
})
const textLimitFields: {key: keyof MemoryConfig, label: string}[] = [
  {key: 'conversationSummaryMaxChars', label: '会话聊天摘要（每条）'},
  {key: 'personMemoryEntryMaxChars', label: '人物记忆提取（每条）'},
  {key: 'longTermMemoryEntryMaxChars', label: '长期记忆（每条）'},
  {key: 'personProfileEntryMaxChars', label: '人物档案（每条）'},
  {key: 'personProfileMaxChars', label: '人物档案（合计）'},
  {key: 'personRelationshipEntryMaxChars', label: '与机器人的关系（每条）'},
  {key: 'personRelationshipMaxChars', label: '与机器人的关系（合计）'},
  {key: 'personInteractionEntryMaxChars', label: '近期互动（每条）'},
  {key: 'personInteractionMaxChars', label: '近期互动（合计）'},
]
const saving: Ref<boolean> = ref(false)

const loading = ref(false)
const loaded = ref(false)
const loadError = ref('')
const savedConfig = ref<string | null>(null)
const isDirty = (): boolean => savedConfig.value !== null && JSON.stringify(memoryConfig) !== savedConfig.value
useUnsavedChanges(isDirty, () => saving.value)
const loadConfig = async (): Promise<void> => {
  if (loading.value || saving.value || !confirmDiscard(isDirty())) return
  loading.value = true
  try {
    const data = await requestJson<MemoryConfig>('/admin/api/memory-config')
    if (!data || typeof data.contextWindowLimit !== 'number') throw new Error('记忆配置数据无效')
    Object.assign(memoryConfig, data)
    loaded.value = true
    savedConfig.value = JSON.stringify(memoryConfig)
    loadError.value = ''
  } catch (cause) {
    loadError.value = requestError(cause, '记忆配置加载失败')
  } finally {
    loading.value = false
  }
}
onMounted(loadConfig)

const {errors, fieldAttrs, validateForm} = useFormValidation(() => {
  const result: Record<string, string> = {}
  for (const [key, value] of Object.entries(memoryConfig)) {
    if (key === 'affinityEnabled') continue
    const threshold = key === 'longTermRecallThreshold' || key === 'longTermInjectThreshold'
    result[`memory-${key}`] = numberError(value, threshold || key === 'memorySummaryContextCount' ? 0 : 1, threshold ? 1 : I32_MAX, !threshold)
    if (threshold && typeof value === 'number' && (value <= 0 || value >= 1)) result[`memory-${key}`] = '请输入大于 0 且小于 1 的数值'
  }
  if (memoryConfig.memorySummaryBatchSize > memoryConfig.memorySummaryTriggerCount) result['memory-memorySummaryBatchSize'] = '每批总结条数不能超过总结触发条数'
  if (memoryConfig.routerWindowKeepCount >= memoryConfig.routerWindowTriggerCount) result['memory-routerWindowKeepCount'] = '保留条数必须小于触发条数'
  return result
})

const saveMemoryConfig = async (): Promise<void> => {
  if (saving.value || loading.value || !loaded.value || !validateForm()) return
  const submitted = JSON.stringify(memoryConfig)
  saving.value = true
  try {
    const resp = await fetch('/admin/api/memory-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: submitted
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      savedConfig.value = submitted
      showToast!('记忆配置已保存')
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '记忆配置保存失败'), true)
  } finally {
    saving.value = false
  }
}
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">记忆与上下文</h1>
      <p class="page-subtitle">配置记忆系统与回复上下文参数</p>
    </div>

    <RequestError :message="loadError" :busy="loading" @retry="loadConfig"/>
    <p v-if="loading" role="status">加载中…</p>
    <form v-if="loaded" class="card" novalidate @submit.prevent="saveMemoryConfig">
      <div class="card-header">
        <h3 class="card-title">参数设置</h3>
      </div>
      <div class="form-group">
        <label class="form-checkbox"><input v-model="memoryConfig.affinityEnabled" type="checkbox" :disabled="saving" aria-describedby="memory-affinityEnabled-hint">启用好感度（保存后生效）</label>
        <p id="memory-affinityEnabled-hint" class="form-hint">关闭后停止评分，模型和人物页面不再使用好感度；历史分数保留，重新开启后恢复。</p>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-contextWindowLimit" class="form-label">近期上下文上限</label>
          <input v-bind="fieldAttrs('memory-contextWindowLimit', 'memory-contextWindowLimit-hint')" :disabled="saving" v-model.number="memoryConfig.contextWindowLimit" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-contextWindowLimit" :message="errors['memory-contextWindowLimit']"/>
          <p id="memory-contextWindowLimit-hint" class="form-hint">1～2147483647，整数。Jev、Router 与 Agent 实际可见的最近消息数上限</p>
        </div>
        <div class="form-group">
          <label for="memory-memorySummaryTriggerCount" class="form-label">总结触发条数</label>
          <input v-bind="fieldAttrs('memory-memorySummaryTriggerCount', 'memory-memorySummaryTriggerCount-hint')" :disabled="saving" v-model.number="memoryConfig.memorySummaryTriggerCount" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-memorySummaryTriggerCount" :message="errors['memory-memorySummaryTriggerCount']"/>
          <p id="memory-memorySummaryTriggerCount-hint" class="form-hint">1～2147483647，整数。完整消息达到该数量时创建一批记忆总结任务</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-memorySummaryBatchSize" class="form-label">每批总结条数</label>
          <input v-bind="fieldAttrs('memory-memorySummaryBatchSize', 'memory-memorySummaryBatchSize-hint')" :disabled="saving" v-model.number="memoryConfig.memorySummaryBatchSize" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-memorySummaryBatchSize" :message="errors['memory-memorySummaryBatchSize']"/>
          <p id="memory-memorySummaryBatchSize-hint" class="form-hint">1～2147483647，整数。成功总结后删除的最旧消息数，不能超过触发条数</p>
        </div>
        <div class="form-group">
          <label for="memory-memorySummaryContextCount" class="form-label">总结补充上下文条数</label>
          <input v-bind="fieldAttrs('memory-memorySummaryContextCount', 'memory-memorySummaryContextCount-hint')" :disabled="saving" v-model.number="memoryConfig.memorySummaryContextCount" class="form-input" type="number" min="0" :max="I32_MAX" step="1">
          <FieldError field="memory-memorySummaryContextCount" :message="errors['memory-memorySummaryContextCount']"/>
          <p id="memory-memorySummaryContextCount-hint" class="form-hint">0～2147483647，整数。仅帮助理解被总结消息的后续语境，不会被提取或删除</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-routerWindowTriggerCount" class="form-label">Router 窗口触发条数</label>
          <input v-bind="fieldAttrs('memory-routerWindowTriggerCount', 'memory-routerWindowTriggerCount-hint')" :disabled="saving" v-model.number="memoryConfig.routerWindowTriggerCount" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-routerWindowTriggerCount" :message="errors['memory-routerWindowTriggerCount']"/>
          <p id="memory-routerWindowTriggerCount-hint" class="form-hint">1～2147483647，整数。Router 上下文超过 N 条时批量滑动（建议 40~60，前缀需超过缓存阈值）</p>
        </div>
        <div class="form-group">
          <label for="memory-routerWindowKeepCount" class="form-label">Router 窗口保留条数</label>
          <input v-bind="fieldAttrs('memory-routerWindowKeepCount', 'memory-routerWindowKeepCount-hint')" :disabled="saving" v-model.number="memoryConfig.routerWindowKeepCount" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-routerWindowKeepCount" :message="errors['memory-routerWindowKeepCount']"/>
          <p id="memory-routerWindowKeepCount-hint" class="form-hint">1～2147483647，整数。滑动后保留的条数，必须小于触发条数</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-jevWindowSize" class="form-label">Jev 窗口条数</label>
          <input v-bind="fieldAttrs('memory-jevWindowSize', 'memory-jevWindowSize-hint')" :disabled="saving" v-model.number="memoryConfig.jevWindowSize" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-jevWindowSize" :message="errors['memory-jevWindowSize']"/>
          <p id="memory-jevWindowSize-hint" class="form-hint">1～2147483647，整数。每次取最近 N 条消息判断是否回复，默认 10 条；受近期上下文上限限制</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-longTermRecallThreshold" class="form-label">长期记忆召回阈值</label>
          <input v-bind="fieldAttrs('memory-longTermRecallThreshold', 'memory-longTermRecallThreshold-hint')" :disabled="saving" v-model.number="memoryConfig.longTermRecallThreshold" class="form-input"
                 type="number" min="0" :max="1" step="any">
          <FieldError field="memory-longTermRecallThreshold" :message="errors['memory-longTermRecallThreshold']"/>
          <p id="memory-longTermRecallThreshold-hint" class="form-hint">新记忆召回长期记忆做合并去重的相似度阈值（大于 0 且小于 1，越高越严格）</p>
        </div>
        <div class="form-group">
          <label for="memory-longTermInjectThreshold" class="form-label">长期记忆注入阈值</label>
          <input v-bind="fieldAttrs('memory-longTermInjectThreshold', 'memory-longTermInjectThreshold-hint')" :disabled="saving" v-model.number="memoryConfig.longTermInjectThreshold" class="form-input"
                 type="number" min="0" :max="1" step="any">
          <FieldError field="memory-longTermInjectThreshold" :message="errors['memory-longTermInjectThreshold']"/>
          <p id="memory-longTermInjectThreshold-hint" class="form-hint">消息入库时召回长期记忆、注入提示词的相似度阈值（大于 0 且小于 1，越高越严格）</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label for="memory-sharedConversationMessageCount" class="form-label">每会话共享消息数</label>
          <input v-bind="fieldAttrs('memory-sharedConversationMessageCount', 'memory-sharedConversationMessageCount-hint')" :disabled="saving" v-model.number="memoryConfig.sharedConversationMessageCount" class="form-input"
                 type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-sharedConversationMessageCount" :message="errors['memory-sharedConversationMessageCount']"/><p id="memory-sharedConversationMessageCount-hint" class="form-hint">1～2147483647 的整数</p>
        </div>
      </div>
      <div class="form-group">
        <label for="memory-conversationSummaryMaxCount" class="form-label">会话聊天摘要保留条数</label>
        <input v-bind="fieldAttrs('memory-conversationSummaryMaxCount', 'memory-conversationSummaryMaxCount-hint')" :disabled="saving" v-model.number="memoryConfig.conversationSummaryMaxCount" class="form-input" type="number" min="1" :max="I32_MAX" step="1">
          <FieldError field="memory-conversationSummaryMaxCount" :message="errors['memory-conversationSummaryMaxCount']"/>
        <p id="memory-conversationSummaryMaxCount-hint" class="form-hint">1～2147483647，整数。每次消息总结时生成摘要，按时间保留每个会话最近 N 条，放在回复上下文的历史消息之前。</p>
      </div>
      <h3 class="card-title">总结篇幅（字）</h3>
      <p class="form-hint">字数为模型生成的目标上限，实际长度受模型遵守指令的程度影响，可能超出。程序不会强行截断。</p>
      <div class="form-row">
        <div v-for="field in textLimitFields" :key="field.key" class="form-group">
          <label class="form-label" :for="`memory-${field.key}`">{{ field.label }}</label>
          <input v-model.number="memoryConfig[field.key]" v-bind="fieldAttrs(`memory-${field.key}`, `memory-${field.key}-hint`)" :disabled="saving" class="form-input" min="1" :max="I32_MAX" step="1" type="number">
          <FieldError :field="`memory-${field.key}`" :message="errors[`memory-${field.key}`]"/>
          <p :id="`memory-${field.key}-hint`" class="form-hint">1～2147483647 字，整数。</p>
        </div>
      </div>
      <button :disabled="saving" class="btn btn-primary" type="submit">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </form>
  </div>
</template>
