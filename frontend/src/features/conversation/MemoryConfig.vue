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
import FieldHelp from '../../components/FieldHelp.vue'
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
  personCompressionTriggerTokens: 0,
  personCompressionTargetTokens: 1500,
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
type NumericMemoryKey = Exclude<keyof MemoryConfig, 'affinityEnabled'>
interface MemoryField {key: NumericMemoryKey, label: string, minimum: number, help: string, threshold?: boolean}
const settingSections: {title: string, summary: string, fields: MemoryField[]}[] = [
  {
    title: '上下文窗口',
    summary: '',
    fields: [
      {key: 'contextWindowLimit', label: '近期上下文上限', minimum: 1, help: 'Jev、Router 与 Agent 实际可见的最近消息数上限'},
      {key: 'jevWindowSize', label: 'Jev 窗口条数', minimum: 1, help: '每次取最近 N 条消息判断是否回复，默认 10 条；受近期上下文上限限制'},
      {key: 'routerWindowTriggerCount', label: 'Router 窗口触发条数', minimum: 1, help: 'Router 上下文超过 N 条时批量滑动（建议 40~60，前缀需超过缓存阈值）'},
      {key: 'routerWindowKeepCount', label: 'Router 窗口保留条数', minimum: 1, help: '滑动后保留的条数，必须小于触发条数'},
      {key: 'sharedConversationMessageCount', label: '每会话共享消息数', minimum: 1, help: ''},
    ]
  },
  {
    title: '记忆总结',
    summary: '成功总结后会删除对应的最旧消息；每批总结条数不能超过触发条数。',
    fields: [
      {key: 'memorySummaryTriggerCount', label: '总结触发条数', minimum: 1, help: '完整消息达到该数量时创建一批记忆总结任务'},
      {key: 'memorySummaryBatchSize', label: '每批总结条数', minimum: 1, help: '成功总结后删除的最旧消息数，不能超过触发条数'},
      {key: 'memorySummaryContextCount', label: '总结补充上下文条数', minimum: 0, help: '仅帮助理解被总结消息的后续语境，不会被提取或删除'},
      {key: 'conversationSummaryMaxCount', label: '会话聊天摘要保留条数', minimum: 1, help: '每次消息总结时生成摘要，按时间保留每个会话最近 N 条，放在回复上下文的历史消息之前。'},
    ]
  },
  {
    title: '长期记忆',
    summary: '',
    fields: [
      {key: 'longTermRecallThreshold', label: '长期记忆召回阈值', minimum: 0, help: '新记忆召回长期记忆做合并去重的相似度阈值（大于 0 且小于 1，越高越严格）', threshold: true},
      {key: 'longTermInjectThreshold', label: '长期记忆注入阈值', minimum: 0, help: '消息入库时召回长期记忆、注入提示词的相似度阈值（大于 0 且小于 1，越高越严格）', threshold: true},
    ]
  },
  {
    title: '人物记忆超限压缩',
    summary: '触发值为 0 时关闭。目标为篇幅参考，实际结果可能超出。',
    fields: [
      {key: 'personCompressionTriggerTokens', label: '压缩触发 Token（0 关闭）', minimum: 0, help: '每日人物整理后执行，使用 Memory 模型；已有统计为 0 的人物不补算。'},
      {key: 'personCompressionTargetTokens', label: '压缩目标 Token', minimum: 0, help: '启用压缩时，目标必须大于 0 且小于触发 Token。'},
    ]
  },
]
const hintsDismissed = ref(false)
const textLimitFields: {key: NumericMemoryKey, label: string}[] = [
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
    result[`memory-${key}`] = numberError(value, threshold || key === 'memorySummaryContextCount' || key === 'personCompressionTriggerTokens' || key === 'personCompressionTargetTokens' ? 0 : 1, threshold ? 1 : I32_MAX, !threshold)
    if (threshold && typeof value === 'number' && (value <= 0 || value >= 1)) result[`memory-${key}`] = '请输入大于 0 且小于 1 的数值'
  }
  if (memoryConfig.memorySummaryBatchSize > memoryConfig.memorySummaryTriggerCount) result['memory-memorySummaryBatchSize'] = '每批总结条数不能超过总结触发条数'
  if (memoryConfig.routerWindowKeepCount >= memoryConfig.routerWindowTriggerCount) result['memory-routerWindowKeepCount'] = '保留条数必须小于触发条数'
  if (memoryConfig.personCompressionTriggerTokens > 0 && (memoryConfig.personCompressionTargetTokens <= 0 || memoryConfig.personCompressionTargetTokens >= memoryConfig.personCompressionTriggerTokens)) result['memory-personCompressionTargetTokens'] = '目标必须大于 0 且小于触发 Token'
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
    <form v-if="loaded" class="memory-form" :class="{'hints-dismissed': hintsDismissed}" novalidate
          @keydown.esc="hintsDismissed = true" @submit.prevent="saveMemoryConfig">
      <section class="memory-section">
        <h2 class="card-title section-heading">好感度</h2>
        <div class="setting-row">
          <div class="setting-description">
            <div class="setting-name">
              <label for="memory-affinityEnabled">启用好感度</label>
              <FieldHelp id="memory-affinityEnabled-details" label="好感度">关闭后停止评分，模型和人物页面不再使用好感度；历史分数保留，重新开启后恢复。</FieldHelp>
            </div>
            <p id="memory-affinityEnabled-hint" class="setting-summary">关闭后停止使用好感度，历史分数保留。</p>
          </div>
          <input id="memory-affinityEnabled" v-model="memoryConfig.affinityEnabled" type="checkbox" :disabled="saving" aria-describedby="memory-affinityEnabled-hint">
        </div>
      </section>
      <section v-for="section in settingSections" :key="section.title" class="memory-section">
        <h2 class="card-title section-heading">{{ section.title }}</h2>
        <p v-if="section.summary" class="section-summary">{{ section.summary }}</p>
        <div class="settings-list">
          <div v-for="field in section.fields" :key="field.key" class="number-row" @focusin="hintsDismissed = false">
            <div class="setting-name">
              <label class="form-label" :for="`memory-${field.key}`">{{ field.label }}</label>
              <FieldHelp v-if="field.help" :id="`memory-${field.key}-details`" :label="field.label">{{ field.help }}</FieldHelp>
            </div>
            <input v-bind="fieldAttrs(`memory-${field.key}`, `memory-${field.key}-hint`)" v-model.number="memoryConfig[field.key]"
                   :disabled="saving" class="form-input" type="number" :min="field.minimum" :max="field.threshold ? 1 : I32_MAX" :step="field.threshold ? 'any' : 1">
            <p :id="`memory-${field.key}-hint`" class="range-hint" role="tooltip">{{ field.threshold ? '大于 0 且小于 1，越高越严格' : `${field.minimum}～${I32_MAX}，整数` }}</p>
            <FieldError :field="`memory-${field.key}`" :message="errors[`memory-${field.key}`]"/>
          </div>
        </div>
      </section>
      <section class="memory-section">
        <h2 class="card-title section-heading">总结篇幅（字）</h2>
        <p class="section-summary">字数为模型生成的目标上限，实际可能超出，程序不会强行截断。</p>
        <div class="settings-list">
          <div v-for="field in textLimitFields" :key="field.key" class="number-row" @focusin="hintsDismissed = false">
            <label class="form-label" :for="`memory-${field.key}`">{{ field.label }}</label>
            <input v-model.number="memoryConfig[field.key]" v-bind="fieldAttrs(`memory-${field.key}`, `memory-${field.key}-hint`)" :disabled="saving" class="form-input" min="1" :max="I32_MAX" step="1" type="number">
            <p :id="`memory-${field.key}-hint`" class="range-hint" role="tooltip">1～{{ I32_MAX }} 字，整数</p>
            <FieldError :field="`memory-${field.key}`" :message="errors[`memory-${field.key}`]"/>
          </div>
        </div>
      </section>
      <div class="save-actions">
        <button :disabled="saving" class="btn btn-primary" type="submit">{{ saving ? '保存中...' : '保存配置' }}</button>
        <p class="save-hint" :class="{'has-changes': isDirty()}" role="status">{{ isDirty() ? '有未保存的修改' : '修改后需保存' }}</p>
      </div>
    </form>
  </div>
</template>

<style scoped>
.memory-form { display: grid; gap: 16px; }
.memory-section { min-width: 0; padding: 20px 24px; background: var(--card-bg); border-radius: var(--radius-md); }
.section-heading { margin-bottom: 12px; color: var(--text-secondary); }
.section-summary, .setting-summary, .save-hint { margin: 0; font-size: 12px; color: var(--text-secondary); }
.section-summary { margin-bottom: 16px; }
.settings-list { display: grid; gap: 12px; }
.number-row { position: relative; display: grid; grid-template-columns: minmax(0, 220px) minmax(0, 160px) minmax(0, 1fr); align-items: center; gap: 8px 16px; }
.number-row + .number-row::before { content: ''; position: absolute; top: -6px; inset-inline-start: 0; width: min(396px, 100%); border-top: 1px solid color-mix(in srgb, var(--border) 35%, transparent); pointer-events: none; }
.form-label { margin: 0; }
.number-row .form-input { min-width: 0; height: 36px; padding: 6px 10px; line-height: 22px; background: color-mix(in srgb, var(--input-bg) 75%, var(--card-bg)); }
.range-hint { margin: 0; font-size: 12px; color: var(--text-secondary); visibility: hidden; }
.memory-form:not(.hints-dismissed) .number-row:focus-within > .range-hint { visibility: visible; }
.number-row :deep(.form-error) { grid-column: 2 / -1; margin: 0; }
.setting-row { position: relative; display: flex; align-items: center; justify-content: space-between; gap: 16px; }
.setting-description { min-width: 0; }
.setting-name { display: flex; align-items: center; gap: 8px; min-width: 0; }
.setting-name label { min-width: 0; }
.setting-row > input { flex-shrink: 0; }
.setting-summary { margin-top: 4px; }
.save-actions { display: flex; align-items: center; flex-wrap: wrap; gap: 8px 12px; }
.save-hint.has-changes { color: var(--warning); font-weight: 500; }
@media (max-width: 600px) {
  .number-row { grid-template-columns: minmax(0, 1fr) minmax(0, 120px); }
  .range-hint { display: none; }
  .memory-form:not(.hints-dismissed) .number-row:focus-within > .range-hint { display: block; grid-column: 1 / -1; }
  .number-row :deep(.form-error) { grid-column: 1 / -1; }
}
</style>
