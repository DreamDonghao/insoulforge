<script lang="ts" setup>
/**
 * @file RequestDebug.vue
 * @brief 请求调试 - 查看最近 HTTP 请求的完整请求/响应体
 */
import {computed, inject, onMounted, onUnmounted, ref} from 'vue'
import type {HttpTraceEntry, HttpTraceListResult} from '../../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const entries = ref<HttpTraceEntry[]>([])
const total = ref(0)
const loading = ref(false)
const keyword = ref('')
const autoRefresh = ref(true)
const expandNestedJson = ref(true)
const selected = ref<HttpTraceEntry | null>(null)
let timer: number | undefined

// 会话展示辅助：私聊会话 ID 带标志位（超过 JS Number 安全范围），需按 BigInt 处理
const PRIVATE_FLAG = 1n << 63n
const sessionLabel = (groupId: string | null | undefined): string => {
  if (!groupId) return '系统请求'
  try {
    return (BigInt(groupId) & PRIVATE_FLAG) !== 0n
        ? `私聊 ${BigInt(groupId) & ~PRIVATE_FLAG}`
        : `群 ${groupId}`
  } catch {
    return groupId
  }
}

const filtered = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  if (!kw) return entries.value
  return entries.value.filter(e =>
      `${e.tag} ${e.url} ${e.groupId ?? ''}`.toLowerCase().includes(kw))
})

// 展开嵌套 JSON 的递归深度上限，防极端深层结构
const kMaxExpandDepth = 32

// 尝试把形如 JSON 对象/数组的字符串解析出来，原始类型或解析失败返回 null
const tryParseNested = (text: string): object | null => {
  const trimmed = text.trim()
  const head = trimmed.charAt(0)
  if (trimmed.length < 2 || (head !== '{' && head !== '[')) return null
  try {
    const parsed: unknown = JSON.parse(trimmed)
    return parsed !== null && typeof parsed === 'object' ? parsed : null
  } catch {
    return null
  }
}

// 递归展开字符串形式的嵌套 JSON（消息 content、工具调用 arguments 等），仅用于展示
const expandNested = (node: unknown, depth: number): unknown => {
  if (depth <= 0) return node
  if (typeof node === 'string') {
    const parsed = tryParseNested(node)
    return parsed ? expandNested(parsed, depth - 1) : node
  }
  if (Array.isArray(node)) return node.map(item => expandNested(item, depth - 1))
  if (node !== null && typeof node === 'object') {
    const out: Record<string, unknown> = {}
    for (const [key, value] of Object.entries(node)) {
      out[key] = expandNested(value, depth - 1)
    }
    return out
  }
  return node
}

// JSON 报文格式化显示，解析失败时原样返回；开启时把字符串形式的嵌套 JSON 展开显示
const pretty = (text: string | null | undefined): string => {
  if (!text) return ''
  try {
    const parsed: unknown = JSON.parse(text)
    return JSON.stringify(expandNestedJson.value ? expandNested(parsed, kMaxExpandDepth) : parsed, null, 2)
  } catch {
    return text
  }
}

const sizeOf = (text: string | null | undefined): number => text?.length ?? 0

const statusClass = (status: number): string =>
    status === 0 ? 'st-none' : status < 400 ? 'st-ok' : 'st-err'

const statusText = (status: number): string =>
    status === 0 ? '无响应' : String(status)

const shortUrl = (url: string): string => url.replace(/^https?:\/\//, '')

const copyText = async (text: string, label: string): Promise<void> => {
  if (!text) return
  try {
    await navigator.clipboard.writeText(text)
    showToast?.(`${label}已复制`)
  } catch {
    showToast?.('复制失败', true)
  }
}

const load = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch('/admin/api/http-traces?limit=100')
    const data: HttpTraceListResult = await resp.json()
    entries.value = data.entries || []
    total.value = data.total || 0
    // 保持选中项跟随最新数据
    if (selected.value) {
      selected.value = entries.value.find(e => e.id === selected.value!.id) ?? selected.value
    }
  } catch {
    showToast?.('请求记录加载失败', true)
  } finally {
    loading.value = false
  }
}

const clearAll = async (): Promise<void> => {
  if (!window.confirm('确定清空所有请求记录？')) return
  try {
    await fetch('/admin/api/http-traces', {method: 'DELETE'})
    selected.value = null
    await load()
    showToast?.('已清空')
  } catch {
    showToast?.('清空失败', true)
  }
}

onMounted(() => {
  void load()
  timer = window.setInterval(() => {
    if (autoRefresh.value) void load()
  }, 5000)
})

onUnmounted(() => window.clearInterval(timer))
</script>

<template>
  <div class="request-debug">
    <div class="page-header">
      <div class="page-title">请求调试</div>
      <div class="page-subtitle">
        {{ entries.length }} / 共 {{ total }} 条 · 内存缓存，重启清空 · status 0 表示未收到响应
      </div>
    </div>

    <div class="card toolbar">
      <input v-model="keyword" class="form-input search" placeholder="筛选 URL / 标签 / 会话 ID">
      <label class="auto-refresh">
        <input v-model="autoRefresh" type="checkbox"> 自动刷新(5s)
      </label>
      <label class="auto-refresh">
        <input v-model="expandNestedJson" type="checkbox"> 展开嵌套 JSON
      </label>
      <button :disabled="loading" class="btn btn-secondary" type="button" @click="load">刷新</button>
      <button class="btn btn-danger" type="button" @click="clearAll">清空</button>
    </div>

    <div class="layout">
      <div class="card trace-list-card">
        <div v-if="filtered.length === 0" class="empty">暂无请求记录</div>
        <div
            v-for="e in filtered"
            :key="e.id"
            :class="{ active: selected?.id === e.id }"
            class="trace-item"
            @click="selected = e"
        >
          <div class="row1">
            <span class="tid">#{{ e.id }}</span>
            <span class="time">{{ e.timestamp }}</span>
            <span :class="['chip', statusClass(e.status)]">{{ statusText(e.status) }}</span>
          </div>
          <div class="row2">
            <span class="tag">{{ e.tag }}</span>
            <span class="method">{{ e.method }}</span>
            <span :title="shortUrl(e.url)" class="url">{{ shortUrl(e.url) }}</span>
          </div>
          <div class="row3">{{ sessionLabel(e.groupId) }} · 请求 {{ sizeOf(e.requestBody) }} / 响应
            {{ sizeOf(e.responseBody) }} 字符
          </div>
        </div>
      </div>

      <div class="card detail-card">
        <template v-if="selected">
          <div class="detail-meta">
            <span :class="['chip', statusClass(selected.status)]">{{ statusText(selected.status) }}</span>
            <span>{{ selected.method }} {{ selected.url }}</span>
            <span>· {{ sessionLabel(selected.groupId) }} · #{{ selected.id }} · {{ selected.timestamp }}</span>
          </div>
          <section class="body-section">
            <div class="body-header">
              <span>请求体（{{ sizeOf(selected.requestBody) }} 字符）</span>
              <button class="btn btn-secondary btn-sm" type="button"
                      @click="copyText(pretty(selected.requestBody), '请求体')">复制
              </button>
            </div>
            <pre class="body-content">{{ pretty(selected.requestBody) || '(无)' }}</pre>
          </section>
          <section class="body-section">
            <div class="body-header">
              <span>响应体（{{ sizeOf(selected.responseBody) }} 字符）</span>
              <button class="btn btn-secondary btn-sm" type="button"
                      @click="copyText(pretty(selected.responseBody), '响应体')">复制
              </button>
            </div>
            <pre class="body-content">{{ pretty(selected.responseBody) || '(无)' }}</pre>
          </section>
        </template>
        <div v-else class="empty">点击左侧请求查看完整内容</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
}

.search {
  max-width: 320px;
}

.auto-refresh {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 13px;
  white-space: nowrap;
}

.layout {
  display: grid;
  grid-template-columns: minmax(360px, 5fr) 7fr;
  gap: 16px;
  align-items: start;
}

@media (max-width: 1100px) {
  .layout {
    grid-template-columns: 1fr;
  }
}

.trace-list-card {
  max-height: calc(100vh - 240px);
  overflow-y: auto;
  padding: 8px;
}

.trace-item {
  padding: 10px 12px;
  border-radius: 8px;
  cursor: pointer;
  border: 1px solid transparent;
}

.trace-item:hover {
  background: var(--row-hover);
}

.trace-item.active {
  background: var(--primary-soft);
  border-color: var(--primary);
}

.row1,
.row2,
.detail-meta {
  display: flex;
  align-items: center;
  gap: 8px;
}

.tid {
  font-weight: 600;
  color: var(--text-secondary);
  font-size: 12px;
}

.time {
  flex: 1;
  font-size: 12px;
  color: var(--text-light);
}

.chip {
  padding: 1px 8px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  white-space: nowrap;
}

.st-ok {
  background: var(--success-soft);
  color: var(--success);
}

.st-err {
  background: var(--danger-soft);
  color: var(--danger);
}

.st-none {
  background: var(--warning-soft);
  color: var(--warning);
}

.tag {
  background: var(--primary-soft);
  color: var(--primary-strong);
  padding: 1px 8px;
  border-radius: 6px;
  font-size: 12px;
  white-space: nowrap;
}

.method {
  font-size: 12px;
  font-weight: 700;
  color: var(--text-secondary);
}

.url {
  font-size: 13px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  direction: rtl;
  text-align: left;
}

.row3 {
  margin-top: 4px;
  font-size: 12px;
  color: var(--text-light);
}

.detail-card {
  padding: 16px;
}

.detail-meta {
  flex-wrap: wrap;
  font-size: 13px;
  color: var(--text-secondary);
  padding-bottom: 12px;
  border-bottom: 1px solid var(--border);
}

.body-section {
  margin-top: 14px;
}

.body-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 6px;
  font-size: 13px;
  font-weight: 600;
  color: var(--text-secondary);
}

.body-content {
  margin: 0;
  padding: 12px;
  max-height: calc((100vh - 320px) / 2);
  overflow: auto;
  background: var(--code-bg);
  color: var(--code-text);
  border-radius: 8px;
  font-size: 12px;
  line-height: 1.5;
  white-space: pre-wrap;
  word-break: break-all;
}

.empty {
  padding: 40px;
  text-align: center;
  color: var(--text-light);
}
</style>
