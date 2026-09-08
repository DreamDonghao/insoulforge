<script lang="ts" setup>
/**
 * @file MemoryConfig.vue
 * @brief 记忆配置组件
 */
import {inject, onMounted, reactive, ref, type Ref} from 'vue'
import type {ApiResponse, MemoryConfig} from '../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const memoryConfig = reactive<MemoryConfig>({
  contextWindowLimit: 100,
  memorySummaryTriggerCount: 100,
  memorySummaryBatchSize: 50,
  memorySummaryContextCount: 10,
  memoryExtractMaxTokens: 4000,
  routerWindowTriggerCount: 20,
  routerWindowKeepCount: 10,
  shortTermMemoryMax: 15,
  longTermRecallThreshold: 0.65,
  longTermInjectThreshold: 0.45
})
const saving: Ref<boolean> = ref(false)

onMounted(async () => {
  const resp = await fetch('/admin/api/memory-config')
  const data = await resp.json()
  if (data.contextWindowLimit !== undefined) {
    Object.assign(memoryConfig, data)
  }
})

const saveMemoryConfig = async (): Promise<void> => {
  saving.value = true
  try {
    const resp = await fetch('/admin/api/memory-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(memoryConfig)
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('记忆配置已保存')
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
      <h1 class="page-title">记忆与上下文</h1>
      <p class="page-subtitle">配置记忆系统与回复上下文参数</p>
    </div>

    <div class="card">
      <div class="card-header">
        <h3 class="card-title">参数设置</h3>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">近期上下文上限</label>
          <input v-model="memoryConfig.contextWindowLimit" class="form-input" type="number">
          <p class="form-hint">Router 与 Agent 实际可见的最近消息数</p>
        </div>
        <div class="form-group">
          <label class="form-label">总结触发条数</label>
          <input v-model="memoryConfig.memorySummaryTriggerCount" class="form-input" type="number">
          <p class="form-hint">完整消息达到该数量时创建一批记忆总结任务</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">每批总结条数</label>
          <input v-model="memoryConfig.memorySummaryBatchSize" class="form-input" type="number">
          <p class="form-hint">成功总结后删除的最旧消息数，不能超过触发条数</p>
        </div>
        <div class="form-group">
          <label class="form-label">总结补充上下文条数</label>
          <input v-model="memoryConfig.memorySummaryContextCount" class="form-input" min="0" type="number">
          <p class="form-hint">仅帮助理解被总结消息的后续语境，不会被提取或删除</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">记忆提取 maxTokens</label>
          <input v-model="memoryConfig.memoryExtractMaxTokens" class="form-input" type="number">
          <p class="form-hint">记忆提取 LLM 调用的输出 token 上限</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">Router 窗口触发条数</label>
          <input v-model="memoryConfig.routerWindowTriggerCount" class="form-input" type="number">
          <p class="form-hint">Router 上下文超过 N 条时批量滑动（建议 40~60，前缀需超过缓存阈值）</p>
        </div>
        <div class="form-group">
          <label class="form-label">Router 窗口保留条数</label>
          <input v-model="memoryConfig.routerWindowKeepCount" class="form-input" type="number">
          <p class="form-hint">滑动后保留的条数，必须小于触发条数</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">短期记忆上限</label>
          <input v-model="memoryConfig.shortTermMemoryMax" class="form-input" type="number">
          <p class="form-hint">归类整理时短期记忆的条数上限</p>
        </div>
        <div class="form-group">
          <label class="form-label">长期记忆召回阈值</label>
          <input v-model="memoryConfig.longTermRecallThreshold" class="form-input" max="1" min="0" step="0.05"
                 type="number">
          <p class="form-hint">新记忆召回长期记忆做合并去重的相似度阈值（0~1，越高越严格）</p>
        </div>
      </div>
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">长期记忆注入阈值</label>
          <input v-model="memoryConfig.longTermInjectThreshold" class="form-input" max="1" min="0" step="0.05"
                 type="number">
          <p class="form-hint">消息入库时召回长期记忆、注入提示词的相似度阈值（0~1，越高越严格）</p>
        </div>
      </div>
      <button :disabled="saving" class="btn btn-primary" @click="saveMemoryConfig">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </div>
  </div>
</template>
