<script lang="ts" setup>
/**
 * @file EmojiManager.vue
 * @brief 表情包库管理组件 - 展示和编辑 QQ 收藏表情（以实际收藏为基准）
 */
import {computed, inject, nextTick, onMounted, ref, type Ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import ModalDialog from '../../components/ModalDialog.vue'
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
const dialog = ref<'add' | 'delete' | null>(null)
const imageUrl = ref('')
const addSource = ref<'file' | 'url'>('file')
const imageFile = ref<File | null>(null)
const hasAddDraft = (): boolean => !!imageUrl.value || !!imageFile.value
const selectFile = (event: Event): void => {
  imageFile.value = (event.target as HTMLInputElement).files?.[0] ?? null
  mutationError.value = ''
}
const deleteTarget = ref<Emoji | null>(null)
const mutating = ref(false)
const busy = computed(() => saving.value || mutating.value)
const mutationError = ref('')
const editError = ref('')
const errorElement = ref<HTMLElement | null>(null)
const urlInput = ref<HTMLInputElement | null>(null)
useUnsavedChanges(() => isDirty() || (dialog.value === 'add' && hasAddDraft()), () => busy.value)

const openDialog = (mode: 'add' | 'delete', emoji?: Emoji): void => {
  if (busy.value || loading.value || !confirmDiscard(isDirty())) return
  finishEdit()
  imageUrl.value = ''
  imageFile.value = null
  addSource.value = 'file'
  mutationError.value = ''
  deleteTarget.value = emoji ?? null
  dialog.value = mode
}
const closeDialog = (): void => {
  if (mutating.value || !confirmDiscard(dialog.value === 'add' && hasAddDraft())) return
  dialog.value = null
}
const submitMutation = async (): Promise<void> => {
  if (busy.value || !dialog.value) return
  mutationError.value = ''
  const mode = dialog.value
  const url = imageUrl.value.trim()
  if (mode === 'add' && addSource.value === 'file') {
    const file = imageFile.value
    if (!file || !file.size || file.size > 8 * 1024 * 1024) {
      mutationError.value = '请选择非空图片，大小不得超过 8 MiB'
      return
    }
  }
  if (mode === 'add' && addSource.value === 'url') {
    try {
      const parsed = new URL(url)
      if (!['http:', 'https:'].includes(parsed.protocol) || parsed.username || parsed.password || url.length > 4096) throw new Error()
    } catch {
      mutationError.value = '请输入有效的 HTTP(S) 图片直链，不含账号密码，最多4096字符'
      await nextTick()
      urlInput.value?.focus()
      return
    }
  }
  mutating.value = true
  try {
    const form = mode === 'add' && addSource.value === 'file' ? new FormData() : null
    if (form && imageFile.value) form.append('image', imageFile.value)
    await requestJson<ApiResponse>(`/admin/api/emoji/${mode}`, {
      method: 'POST', headers: form ? undefined : {'Content-Type': 'application/json'}, signal: AbortSignal.timeout(90000),
      body: form ?? JSON.stringify(mode === 'add' ? {url} : {res_id: deleteTarget.value?.res_id})
    })
    dialog.value = null
    imageUrl.value = ''
    imageFile.value = null
    showToast?.(mode === 'add' ? '已提交 QQ 收藏，请核对列表' : '已提交 QQ 删除，请核对列表')
    await loadEmojis()
  } catch (cause) {
    mutationError.value = requestError(cause, '操作未完成或结果未确认，请关闭弹窗并刷新列表核对后再试')
    await nextTick()
    errorElement.value?.focus()
  } finally {
    mutating.value = false
  }
}

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
  if (mutating.value || loading.value || !confirmDiscard(isDirty())) return
  editError.value = ''
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
  if (busy.value) return
  editError.value = ''
  const revision = editRevision
  const content = editingDesc.value
  const snapshot = {emoji_id: emoji.emoji_id, res_id: emoji.res_id, md5: emoji.md5, desc: content.trim()}
  saving.value = true
  try {
    await requestJson<ApiResponse>('/admin/api/emoji/desc', {
      method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(snapshot)
    })
    showToast?.('描述已修改')
    const stored = emojis.value.find(item => item.res_id === snapshot.res_id)
    if (stored) stored.summary = snapshot.desc
    if (revision === editRevision) {
      savedDescription.value = content
      if (editingDesc.value === content) finishEdit()
    }
    await loadEmojis()
  } catch (cause) {
    editError.value = requestError(cause, '修改描述失败')
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
      <p class="page-subtitle">管理当前机器人 QQ 账号的真实收藏，添加、修改和删除会同步到 QQ。</p>
    </div>

    <div class="card">
      <div class="card-header">
        <h3 class="card-title">收藏表情 ({{ emojis.length }})</h3>
        <div class="emoji-actions">
          <button :disabled="busy || loading" class="btn btn-secondary" @click="loadEmojis">{{ loading ? '加载中…' : '刷新' }}</button>
          <button :disabled="busy || loading" class="btn btn-primary" @click="openDialog('add')">添加收藏表情</button>
        </div>
      </div>
      <p class="emoji-hint">需要 QQ 在线且 OneBot 实现支持收藏扩展接口。仅显示接口返回的前 200 项；未返回表情也可能是离线或接口不可用。添加支持本地图片上传或图片直链，成功后可编辑描述；QQ 收藏限制、重复图片及同步延迟以客户端为准。</p>
      <RequestError :message="loadError" :busy="loading" :stale="loaded" @retry="loadEmojis"/>
      <p v-if="loading && loaded" role="status">刷新中…</p>
      <div class="table-container">
        <template v-if="loading && !loaded">
          <div class="empty-state"><p>加载中...</p></div>
        </template>
        <template v-else-if="!loadError && emojis.length === 0">
          <div class="empty-state">
            <div class="empty-icon" aria-hidden="true"><NavIcon name="emojis"/></div>
            <p>未返回收藏表情，可添加图片或检查 QQ 连接与接口支持</p>
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
                <p v-if="editError" role="alert">{{ editError }}</p>
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
                  <div class="emoji-actions emoji-row-actions">
                    <button type="button" class="btn btn-secondary btn-sm" :disabled="busy || loading || !emoji.res_id" @click="startEdit(emoji)">编辑</button>
                    <button type="button" class="btn btn-danger btn-sm" :disabled="busy || loading || !emoji.res_id" :aria-label="`删除收藏表情：${emoji.name}`" @click="openDialog('delete', emoji)">删除</button>
                  </div>
                </div>
              </template>
            </div>
          </div>
        </template>
      </div>
    </div>
    <ModalDialog v-if="dialog" class="emoji-overlay" labelledby="emoji-dialog-title" @close="closeDialog">
      <form class="emoji-dialog" novalidate @submit.prevent="submitMutation">
        <h2 id="emoji-dialog-title">{{ dialog === 'add' ? '添加收藏表情' : '删除收藏表情' }}</h2>
        <template v-if="dialog === 'add'">
          <label class="form-label" for="emoji-add-source">添加方式</label>
          <select id="emoji-add-source" v-model="addSource" class="form-input" :disabled="mutating" @change="mutationError = ''">
            <option value="file">本地图片上传</option>
            <option value="url">图片直链</option>
          </select>
          <template v-if="addSource === 'file'">
            <label class="form-label" for="emoji-image-file">图片文件（必填）</label>
            <input id="emoji-image-file" class="form-input" type="file" accept="image/gif,image/png,image/jpeg,image/webp"
                   :disabled="mutating" :aria-invalid="!!mutationError" aria-describedby="emoji-file-help emoji-mutation-error" @change="selectFile">
            <p v-if="imageFile">已选择：{{ imageFile.name }}</p>
            <p id="emoji-file-help" class="emoji-hint">支持 GIF（含动图）、PNG、JPEG、WebP，最大 8 MiB。添加后可编辑描述。</p>
          </template>
          <template v-else>
          <label class="form-label" for="emoji-image-url">图片直链（必填）</label>
          <input id="emoji-image-url" ref="urlInput" v-model="imageUrl" class="form-input" type="url" required
                 :disabled="mutating" :aria-invalid="!!mutationError" aria-describedby="emoji-url-help emoji-mutation-error"
                 placeholder="https://example.com/image.png">
          <p id="emoji-url-help" class="emoji-hint">请输入 HTTP(S) 图片地址，QQ 客户端所在主机必须能直接下载。添加后可编辑描述。</p>
          </template>
        </template>
        <p v-else>将从机器人 QQ 账号的收藏中删除「{{ deleteTarget?.name }}」，所有会话将无法再通过该收藏发送。此操作不能撤销。</p>
        <p v-if="mutationError" id="emoji-mutation-error" ref="errorElement" tabindex="-1" role="alert">{{ mutationError }}</p>
        <p v-if="mutating" role="status">{{ dialog === 'add' ? '正在传输图片并提交 QQ 收藏…' : '正在提交 QQ 删除…' }}请等待请求完成后关闭。</p>
        <div class="emoji-actions emoji-footer">
          <button type="button" class="btn btn-secondary" data-modal-initial-focus :disabled="mutating" @click="closeDialog">取消</button>
          <button type="submit" class="btn" :class="dialog === 'add' ? 'btn-primary' : 'btn-danger'" :disabled="mutating">
            {{ mutating ? '提交中…' : dialog === 'add' ? '添加到 QQ 收藏' : '从 QQ 收藏删除' }}
          </button>
        </div>
      </form>
    </ModalDialog>
  </div>
</template>

<style scoped>
.emoji-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.emoji-row-actions { justify-content: center; margin-top: 8px; }
.emoji-hint { color: var(--text-secondary); font-size: 13px; overflow-wrap: anywhere; }
.emoji-overlay { display: flex; align-items: center; justify-content: center; padding: 16px; background: var(--overlay-bg, rgba(0,0,0,.5)); box-sizing: border-box; }
.emoji-dialog { background: var(--bg-main); border: 1px solid var(--border); padding: 24px; width: min(480px, 100%); max-height: 100%; overflow-y: auto; overflow-wrap: anywhere; }
.emoji-footer { justify-content: flex-end; margin-top: 24px; }
.emoji-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(140px, 100%), 1fr)); gap: 24px 16px; }
.emoji-item { min-width: 0; text-align: center; }
.emoji-preview { display: flex; align-items: center; justify-content: center; height: 100px; background: var(--bg-secondary); border: 1px solid var(--border); }
</style>
