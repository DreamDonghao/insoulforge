<script lang="ts" setup>
/**
 * @file PromptEditor.vue
 * @brief 提示词编辑组件
 */
import {inject, onMounted, onUnmounted, reactive, ref, type Ref, watch} from 'vue'
import type {ApiResponse} from '../../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const promptList = [
  {key: 'executor_system', label: '群聊 · 人设'},
  {key: 'router_system', label: '群聊 · 路由'},
  {key: 'executor_private_system', label: '私聊 · 人设'},
  {key: 'router_private_system', label: '私聊 · 路由'},
  {key: 'character-image', label: '角色形象'},
]
const selectedPrompt: Ref<string> = ref('executor_system')
const prompts = reactive<Record<string, string>>({})
const promptContent: Ref<string> = ref('')
const saving: Ref<boolean> = ref(false)
const imageUrl = ref('')
const imageLoading = ref(true)
const imageSaving = ref(false)
const imageInput = ref<HTMLInputElement | null>(null)

const revokeImageUrl = (): void => {
  if (imageUrl.value) URL.revokeObjectURL(imageUrl.value)
}

const readImageApiResult = async (response: Response): Promise<ApiResponse> => {
  if (response.status === 413) {
    throw new Error('服务器拒绝了上传请求（HTTP 413），请更新并重启后端后重试')
  }
  if (!response.headers.get('content-type')?.includes('application/json')) {
    throw new Error(`角色形象接口不可用（HTTP ${response.status}），请确认后端已更新并重启`)
  }
  return response.json() as Promise<ApiResponse>
}

const loadCharacterImage = async (): Promise<boolean> => {
  imageLoading.value = true
  try {
    const response = await fetch('/admin/api/character-image', {cache: 'no-store'})
    if (response.status === 404 && response.headers.get('content-type')?.includes('application/json')) {
      revokeImageUrl()
      imageUrl.value = ''
      return true
    }
    if (!response.ok) {
      const result = await readImageApiResult(response)
      throw new Error(result.error || `读取角色形象图失败（HTTP ${response.status}）`)
    }
    if (!response.headers.get('content-type')?.startsWith('image/')) {
      throw new Error('角色形象接口返回了非图片内容，请检查后端版本')
    }
    const url = URL.createObjectURL(await response.blob())
    revokeImageUrl()
    imageUrl.value = url
    return true
  } catch (error) {
    showToast!(error instanceof Error ? error.message : '读取角色形象图失败', true)
    return false
  } finally {
    imageLoading.value = false
  }
}

const uploadCharacterImage = async (event: Event): Promise<void> => {
  const file = (event.target as HTMLInputElement).files?.[0]
  if (!file) return
  if (file.size > 8 * 1024 * 1024) {
    showToast!('图片不得超过 8 MiB', true)
    if (imageInput.value) imageInput.value.value = ''
    return
  }
  imageSaving.value = true
  try {
    const body = new FormData()
    body.append('image', file)
    const response = await fetch('/admin/api/character-image', {method: 'POST', body})
    const result = await readImageApiResult(response)
    if (!response.ok || !result.success) throw new Error(result.error || '上传失败')
    if (await loadCharacterImage()) showToast!('角色形象图已保存')
  } catch (error) {
    showToast!(error instanceof Error ? error.message : '上传失败', true)
  } finally {
    imageSaving.value = false
    if (imageInput.value) imageInput.value.value = ''
  }
}

const deleteCharacterImage = async (): Promise<void> => {
  if (!window.confirm('确定删除角色形象图？删除后无法使用参考图绘制自身形象。')) return
  imageSaving.value = true
  try {
    const response = await fetch('/admin/api/character-image', {method: 'DELETE'})
    const result = await readImageApiResult(response)
    if (!response.ok || !result.success) throw new Error(result.error || '删除失败')
    revokeImageUrl()
    imageUrl.value = ''
    showToast!('角色形象图已删除')
  } catch (error) {
    showToast!(error instanceof Error ? error.message : '删除失败', true)
  } finally {
    imageSaving.value = false
  }
}

onMounted(() => { void loadCharacterImage() })
onUnmounted(revokeImageUrl)

watch(selectedPrompt, async (key: string) => {
  if (key === 'character-image') return
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

    <section v-if="selectedPrompt === 'character-image'" class="character-section">
      <div class="character-heading">
        <div>
          <h2>角色形象参考图</h2>
          <p class="form-hint">绘制自身形象时作为参考图；支持 PNG、JPEG、WebP，最大 8 MiB。</p>
        </div>
        <div class="character-actions">
          <input ref="imageInput" accept="image/png,image/jpeg,image/webp" class="character-file-input" type="file"
                 @change="uploadCharacterImage">
          <button :disabled="imageSaving" class="btn btn-primary" type="button" @click="imageInput?.click()">
            {{ imageSaving ? '处理中...' : imageUrl ? '替换图片' : '上传图片' }}
          </button>
          <button v-if="imageUrl" :disabled="imageSaving" class="btn btn-danger" type="button"
                  @click="deleteCharacterImage">删除</button>
        </div>
      </div>
      <div v-if="imageLoading" class="character-empty">加载中...</div>
      <img v-else-if="imageUrl" :src="imageUrl" alt="当前角色形象参考图" class="character-preview">
      <div v-else class="character-empty">尚未上传角色形象图</div>
    </section>

    <div v-else class="card prompt-card">
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

<style scoped>
.character-section {
  border-top: 1px solid var(--border);
  padding-top: 24px;
}

.character-heading {
  display: flex;
  align-items: start;
  justify-content: space-between;
  gap: 20px;
  flex-wrap: wrap;
}

.character-heading h2 {
  font-size: 18px;
  margin: 0 0 8px;
}

.character-actions {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.character-file-input {
  display: none;
}

.character-preview {
  display: block;
  max-width: min(100%, 600px);
  max-height: 560px;
  object-fit: contain;
  margin-top: 24px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.character-empty {
  display: grid;
  place-items: center;
  max-width: 600px;
  min-height: 240px;
  margin-top: 24px;
  border: 1px dashed var(--border);
  border-radius: 8px;
  color: var(--text-secondary);
}
</style>
