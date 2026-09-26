<script lang="ts" setup>
/**
 * @file PromptEditor.vue
 * @brief 提示词编辑组件
 */
import {inject, reactive, ref, type Ref, watch} from 'vue'
import type {ApiResponse} from '../../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const promptList = [
  {key: 'executor_system', label: '群聊 · 人设'},
  {key: 'router_system', label: '群聊 · 路由'},
  {key: 'executor_private_system', label: '私聊 · 人设'},
  {key: 'router_private_system', label: '私聊 · 路由'},
]
const selectedPrompt: Ref<string> = ref('executor_system')
const prompts = reactive<Record<string, string>>({})
const promptContent: Ref<string> = ref('')
const saving: Ref<boolean> = ref(false)

watch(selectedPrompt, async (key: string) => {
  if (Object.keys(prompts).length === 0) {
    const resp = await fetch('/admin/api/prompts')
    Object.assign(prompts, await resp.json())
  }
  promptContent.value = prompts[key] || ''
}, {immediate: true})

const savePrompt = async (): Promise<void> => {
  saving.value = true
  try {
    const resp = await fetch('/admin/api/prompt', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({key: selectedPrompt.value, content: promptContent.value})
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('提示词已保存')
      prompts[selectedPrompt.value] = promptContent.value
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
      <h1 class="page-title">提示词管理</h1>
      <p class="page-subtitle">编辑Agent的系统提示词</p>
    </div>

    <div class="tabs">
      <button
          v-for="p in promptList"
          :key="p.key"
          :class="{ active: selectedPrompt === p.key }"
          class="tab"
          @click="selectedPrompt = p.key"
      >{{ p.label }}
      </button>
    </div>

    <div class="card prompt-card">
      <div class="card-body">
        <textarea v-model="promptContent" class="form-input" placeholder="输入提示词内容..."></textarea>
        <div style="margin-top: 16px;">
          <button :disabled="saving" class="btn btn-primary" @click="savePrompt">
            {{ saving ? '保存中...' : '保存提示词' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>