<script lang="ts" setup>
/**
 * @file EmojiManager.vue
 * @brief 表情包库管理组件 - 展示和编辑 QQ 收藏表情（以实际收藏为基准）
 */
import {inject, onMounted, ref, type Ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import NavIcon from '../../components/NavIcon.vue'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import type {ApiResponse, Emoji} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const emojis: Ref<Emoji[]> = ref([])
const loading: Ref<boolean> = ref(false)
const editingId: Ref<string> = ref('')
const editingDesc: Ref<string> = ref('')
const saving: Ref<boolean> = ref(false)
let editRevision = 0
const savedDescription = ref('')
const isDirty = (): boolean => !!editingId.value && editingDesc.value !== savedDescription.value
useUnsavedChanges(isDirty, () => saving.value)

const loadError = ref('')
const loaded = ref(false)
const loadEmojis = async (): Promise<void> => {
  if (loading.value) return
  loading.value = true
  try {
    const data = await requestJson<Emoji[]>('/admin/api/emojis')
    if (!Array.isArray(data)) throw new Error('表情列表数据无效')
    emojis.value = data
    loaded.value = true
    loadError.value = ''
  } catch (cause) {
    loadError.value = requestError(cause, '收藏表情加载失败')
  } finally {
    loading.value = false
  }
}

const startEdit = (emoji: Emoji): void => {
  if (!confirmDiscard(isDirty())) return
  editRevision++
  editingId.value = emoji.res_id
  editingDesc.value = emoji.summary || ''
  savedDescription.value = editingDesc.value
}

const finishEdit = (): void => {
  editRevision++
  editingId.value = ''
  editingDesc.value = ''
}

const cancelEdit = (): void => {
  if (saving.value || !confirmDiscard(isDirty())) return
  finishEdit()
}

const saveDesc = async (emoji: Emoji): Promise<void> => {
  if (saving.value) return
  const revision = editRevision
  const content = editingDesc.value
  const snapshot = {emoji_id: emoji.emoji_id, res_id: emoji.res_id, md5: emoji.md5, desc: content.trim()}
  saving.value = true
  try {
    const resp = await fetch('/admin/api/emoji/desc', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(snapshot)
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!('描述已修改')
      const stored = emojis.value.find(item => item.res_id === snapshot.res_id)
      if (stored) stored.summary = snapshot.desc
      if (revision === editRevision) {
        savedDescription.value = content
        if (editingDesc.value === content) finishEdit()
      }
      await loadEmojis()
    } else {
      showToast!(data.error || '修改失败', true)
    }
  } catch {
    showToast!('网络错误，请检查后端服务', true)
  } finally {
    saving.value = false
  }
}

onMounted(loadEmojis)
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">收藏表情</h1>
      <p class="page-subtitle">直接展示 QQ 收藏表情，可修改描述；Bot 通过 send_sticker 发送 mface 消息</p>
    </div>

    <div class="card">
      <div class="card-header">
        <h3 class="card-title">收藏表情 ({{ emojis.length }})</h3>
        <button :disabled="loading" class="btn btn-secondary" @click="loadEmojis">
          {{ loading ? '加载中...' : '刷新' }}
        </button>
      </div>
      <RequestError :message="loadError" :busy="loading" :stale="loaded" @retry="loadEmojis"/>
      <p v-if="loading && loaded" role="status">刷新中…</p>
      <div class="table-container">
        <template v-if="loading && !loaded">
          <div class="empty-state"><p>加载中...</p></div>
        </template>
        <template v-else-if="!loadError && emojis.length === 0">
          <div class="empty-state">
            <div class="empty-icon" aria-hidden="true"><NavIcon name="emojis"/></div>
            <p>暂无收藏表情，或 QQ 客户端不在线</p>
          </div>
        </template>
        <template v-else-if="emojis.length > 0">
          <div class="emoji-grid">
            <div v-for="(emoji, i) in emojis" :key="emoji.res_id || i"
                 class="emoji-item">
              <div class="emoji-preview">
                <img v-if="emoji.url" :alt="emoji.name" :src="emoji.url"
                     loading="lazy"
                     style="max-width:100%;max-height:100%;object-fit:contain"
                     @error="($event.target as HTMLImageElement).style.display='none'">
                <span v-if="!emoji.url" style="color:var(--text-muted);font-size:12px">无预览</span>
              </div>

              <template v-if="editingId === emoji.res_id">
                <label :for="`emoji-description-${i}`" class="form-label">表情描述</label>
                <input :id="`emoji-description-${i}`" v-model="editingDesc" :disabled="saving" class="form-input"
                       placeholder="表情描述"
                       style="margin-top:6px;width:100%;box-sizing:border-box" type="text">
                <div style="margin-top:4px;display:flex;gap:6px;justify-content:center">
                  <button :disabled="saving" class="btn btn-primary btn-sm" @click="saveDesc(emoji)">
                    {{ saving ? '保存中' : '保存' }}
                  </button>
                  <button :disabled="saving" class="btn btn-sm" @click="cancelEdit">取消</button>
                </div>
              </template>
              <template v-else>
                <div style="margin-top:6px;font-size:13px;word-break:break-all">{{ emoji.name }}</div>
                <div style="font-size:11px;color:var(--text-muted)">
                  {{ emoji.is_mark_face ? '商城表情' : '自定义表情' }}
                  <button type="button" class="btn btn-secondary btn-sm" style="margin-left:4px" @click="startEdit(emoji)">编辑</button>
                </div>
              </template>
            </div>
          </div>
        </template>
      </div>
    </div>
  </div>
</template>

<style scoped>
.emoji-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(140px, 100%), 1fr)); gap: 24px 16px; }
.emoji-item { min-width: 0; text-align: center; }
.emoji-preview { display: flex; align-items: center; justify-content: center; height: 100px; background: var(--bg-secondary); border: 1px solid var(--border); }
</style>
