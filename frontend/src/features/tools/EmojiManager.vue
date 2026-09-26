<script lang="ts" setup>
/**
 * @file EmojiManager.vue
 * @brief 表情包库管理组件 - 展示和编辑 QQ 收藏表情（以实际收藏为基准）
 */
import {inject, onMounted, ref, type Ref} from 'vue'
import type {ApiResponse, Emoji} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const emojis: Ref<Emoji[]> = ref([])
const loading: Ref<boolean> = ref(false)
const editingId: Ref<string> = ref('')
const editingDesc: Ref<string> = ref('')
const saving: Ref<boolean> = ref(false)

const loadEmojis = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch('/admin/api/emojis')
    if (!resp.ok) {
      showToast!('加载失败: ' + resp.status, true)
      emojis.value = []
      return
    }
    const data = await resp.json()
    emojis.value = Array.isArray(data) ? data : []
    if (emojis.value.length === 0) {
      showToast!('收藏表情为空，或 QQ 客户端不在线', true)
    }
  } catch {
    showToast!('网络错误，请检查后端服务', true)
    emojis.value = []
  } finally {
    loading.value = false
  }
}

const startEdit = (emoji: Emoji): void => {
  editingId.value = emoji.res_id
  editingDesc.value = emoji.summary || ''
}

const cancelEdit = (): void => {
  editingId.value = ''
  editingDesc.value = ''
}

const saveDesc = async (emoji: Emoji): Promise<void> => {
  saving.value = true
  try {
    const resp = await fetch('/admin/api/emoji/desc', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        emoji_id: emoji.emoji_id,
        res_id: emoji.res_id,
        md5: emoji.md5,
        desc: editingDesc.value.trim()
      })
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('描述已修改')
      cancelEdit()
      loadEmojis()
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
        <button :disabled="loading" class="btn btn-success" @click="loadEmojis">
          {{ loading ? '加载中...' : '刷新' }}
        </button>
      </div>
      <div class="table-container">
        <template v-if="loading && emojis.length === 0">
          <div class="empty-state"><p>加载中...</p></div>
        </template>
        <template v-else-if="emojis.length === 0">
          <div class="empty-state">
            <div class="empty-icon">😺</div>
            <p>暂无收藏表情，或 QQ 客户端不在线</p>
          </div>
        </template>
        <template v-else>
          <div style="display:flex;flex-wrap:wrap;gap:16px;padding:16px">
            <div v-for="(emoji, i) in emojis" :key="emoji.res_id || i"
                 style="width:140px;text-align:center">
              <div style="width:100%;height:100px;display:flex;align-items:center;justify-content:center;
                          background:var(--bg-secondary, #f5f5f5);border-radius:8px;
                          border:1px solid var(--border-color)">
                <img v-if="emoji.url" :alt="emoji.name" :src="emoji.url"
                     loading="lazy"
                     style="max-width:100%;max-height:100%;object-fit:contain"
                     @error="($event.target as HTMLImageElement).style.display='none'">
                <span v-if="!emoji.url" style="color:var(--text-muted);font-size:12px">无预览</span>
              </div>

              <template v-if="editingId === emoji.res_id">
                <input v-model="editingDesc" class="form-input"
                       placeholder="表情描述"
                       style="margin-top:6px;width:100%;box-sizing:border-box" type="text">
                <div style="margin-top:4px;display:flex;gap:6px;justify-content:center">
                  <button :disabled="saving" class="btn btn-success btn-sm" @click="saveDesc(emoji)">
                    {{ saving ? '保存中' : '保存' }}
                  </button>
                  <button :disabled="saving" class="btn btn-sm" @click="cancelEdit">取消</button>
                </div>
              </template>
              <template v-else>
                <div style="margin-top:6px;font-size:13px;word-break:break-all">{{ emoji.name }}</div>
                <div style="font-size:11px;color:var(--text-muted)">
                  {{ emoji.is_mark_face ? '商城表情' : '自定义表情' }}
                  <a href="javascript:void(0)" style="margin-left:4px" @click="startEdit(emoji)">编辑</a>
                </div>
              </template>
            </div>
          </div>
        </template>
      </div>
    </div>
  </div>
</template>