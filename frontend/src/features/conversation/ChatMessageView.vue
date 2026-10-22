<script setup lang="ts">
import {computed, ref, watch} from 'vue'
import type {ChatMessage} from '../../vite-env'

const props = defineProps<{message: ChatMessage; botName: string}>()
const emit = defineEmits<{edit: []; delete: []}>()
const expanded = ref(false)

const getString = (value: unknown): string => typeof value === 'string' ? value : ''
const getRecord = (value: unknown): Record<string, unknown> | null =>
    value !== null && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : null

const record = computed(() => {
  try {
    return getRecord(JSON.parse(props.message.content))
  } catch {
    return null
  }
})

const sender = computed(() => getString(getRecord(record.value?.sender)?.name) ||
    (props.message.role === 'user' ? '用户' : props.botName))

/** 从统一资产或兼容的旧版图片字段取得图片识别描述。 */
const imageDescription = (record: Record<string, unknown>, segment: Record<string, unknown>): string => {
  if (getString(segment.description)) return getString(segment.description)

  const index = typeof segment.image_index === 'number' ? segment.image_index : -1
  const assets = getRecord(record.assets)
  const images = Array.isArray(assets?.images) ? assets.images : Array.isArray(record.images) ? record.images : []
  return index >= 0 ? getString(getRecord(images[index])?.description) : ''
}

const text = computed(() => {
  const value = record.value
  if (!value) return props.message.content

  const segments = Array.isArray(value.segments) ? value.segments : []
  const formatted = segments.map(segment => {
    const item = getRecord(segment)
    if (!item) return ''

    switch (getString(item.type)) {
      case 'text':
        return getString(item.text)
      case 'at': {
        const target = getRecord(item.target)
        if (getString(target?.kind) === 'all' || getString(item.qq) === '0') return '@全体成员'
        const name = getString(target?.name) || getString(item.name)
        const qq = getString(target?.qq) || getString(item.qq)
        return name ? `@${name}` : qq ? `@${qq}` : '@成员'
      }
      case 'image': {
        const description = imageDescription(value, item)
        return description ? `[图片] ${description}` : '[图片]'
      }
      case 'face':
        return `[QQ 表情${getString(item.label) ? `：${getString(item.label)}` : ''}]`
      case 'sticker':
        return `[表情包：${getString(item.name) || '未命名'}]`
      case 'poke': {
        const target = getRecord(item.target)
        const name = getString(target?.name) || getString(item.target_name)
        return `[拍一拍${name ? `：${name}` : ''}]`
      }
      case 'notification':
      case 'member_event': {
        const action = getString(item.action) || getString(item.kind)
        return `[成员事件${action ? `：${action}` : ''}]`
      }
      case 'unsupported':
        return `[${getString(item.segment_type) || '未知消息'}]`
      default:
        return `[${getString(item.type) || '未知消息'}]`
    }
  }).join('')

  return formatted || getString(value.text) || '[空消息]'
})

// Computed values are lazy: collapsed messages never format their raw JSON.
const rawJson = computed(() => JSON.stringify(record.value, null, 2))
watch(record, value => { if (!value) expanded.value = false })

const toggleJson = (event: Event): void => {
  expanded.value = (event.target as HTMLDetailsElement).open
}
</script>

<template>
  <div class="msg-header">
    <div class="msg-identity">
      <span class="msg-role">{{ sender }}</span>
      <span v-if="message.role === 'assistant'" class="msg-bot-label">机器人</span>
    </div>
    <div v-if="message.id !== undefined" class="msg-actions">
      <button class="action-btn" title="编辑" @click="emit('edit')">✏️</button>
      <button class="action-btn delete" title="删除" @click="emit('delete')">🗑️</button>
    </div>
  </div>
  <div class="msg-content">{{ text }}</div>
  <details v-if="record" class="message-debug" @toggle="toggleJson">
    <summary>原始 JSON</summary>
    <pre v-if="expanded">{{ rawJson }}</pre>
  </details>
</template>

<style scoped>
.msg-identity {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
}

.msg-bot-label {
  padding: 1px 6px;
  background: var(--primary-soft);
  color: var(--primary);
  font-size: 11px;
}

.msg-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
}

.msg-role {
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0;
  color: var(--text-secondary);
}

.msg-actions {
  display: flex;
  gap: 4px;
  opacity: 0;
  transition: opacity 0.2s;
}

.chat-message:hover .msg-actions,
.chat-message:focus-within .msg-actions {
  opacity: 1;
}

.action-btn {
  background: none;
  border: none;
  cursor: pointer;
  font-size: 14px;
  padding: 2px 4px;
  opacity: 0.6;
}

.action-btn:hover {
  opacity: 1;
}

.action-btn.delete:hover {
  opacity: 1;
  filter: brightness(0) saturate(100%) invert(27%) sepia(94%) saturate(6514%) hue-rotate(355deg) brightness(93%) contrast(127%);
}

.msg-content {
  font-size: 14px;
  line-height: 1.5;
  white-space: pre-wrap;
  word-break: break-word;
}

.message-debug {
  margin-top: 14px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
  color: var(--text-secondary);
}

.message-debug summary {
  cursor: pointer;
  font-size: 12px;
}

.message-debug pre {
  margin: 8px 0 0;
  padding: 12px;
  background: var(--code-bg);
  color: var(--code-text);
  border: 1px solid var(--border);
  border-radius: 0;
  font-size: 12px;
  line-height: 1.6;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  tab-size: 2;
}

@media (hover: none) {
  .msg-actions {
    opacity: 1;
  }
}
</style>
