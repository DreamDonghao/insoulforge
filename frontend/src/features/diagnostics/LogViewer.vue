<script lang="ts" setup>
import {computed, inject, nextTick, onMounted, onUnmounted, ref} from 'vue'
import type {Group, LogEntry, LogQueryResult} from '../../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const groups = ref<Group[]>([])
const entries = ref<LogEntry[]>([])
const loading = ref(false)
const followLatest = ref(true)
const hasMore = ref(false)
const nextBeforeId = ref(0)
const currentLevel = ref('info')
const size = ref(0)
const groupId = ref<'all' | 'system' | string>('all')
const level = ref<'all' | string>('all')
const keyword = ref('')
const limit = ref(200)
const logListEl = ref<HTMLElement | null>(null)
let logWs: WebSocket | null = null
let reconnectTimer: number | undefined

const scrollToBottom = async (): Promise<void> => {
  await nextTick()
  if (logListEl.value) {
    logListEl.value.scrollTop = logListEl.value.scrollHeight
  }
}

// 会话展示辅助：私聊会话 ID 带标志位（超过 JS Number 安全范围），需按字符串/BigInt 处理
const PRIVATE_FLAG = 1n << 63n
const sessionKey = (g: Group): string => g.groupIdStr ?? String(g.groupId)
const sessionLabel = (g: Group): string =>
    g.sessionType === 'private'
        ? g.groupName ? `${g.groupName} (${g.userId ?? ''})` : `私聊 ${g.userId ?? ''}`
        : g.groupName || `群 ${g.groupId}`
const groupTag = (sessionId: string): string => {
  const g = groups.value.find(item => sessionKey(item) === sessionId)
  if (g) return sessionLabel(g)
  try {
    return (BigInt(sessionId) & PRIVATE_FLAG) !== 0n
        ? `私聊 ${BigInt(sessionId) & ~PRIVATE_FLAG}`
        : `群 ${sessionId}`
  } catch {
    return `群 ${sessionId}`
  }
}

const currentGroupLabel = computed(() => {
  if (groupId.value === 'all') return '全部日志'
  if (groupId.value === 'system') return '系统日志'
  const group = groups.value.find(item => sessionKey(item) === groupId.value)
  return group ? sessionLabel(group) : groupTag(groupId.value)
})

const levelClass = (entryLevel: string): string => `level-${entryLevel}`

const subscribe = (): void => {
  if (!logWs || logWs.readyState !== WebSocket.OPEN) return
  logWs.send(JSON.stringify({
    action: 'subscribe',
    groupId: groupId.value,
    level: level.value,
    keyword: keyword.value.trim()
  }))
}

const connectWebSocket = (): void => {
  if (logWs && logWs.readyState <= WebSocket.OPEN) return
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
  logWs = new WebSocket(`${protocol}//${location.host}/admin/logs/ws`)
  logWs.onopen = subscribe
  logWs.onmessage = handleWsMessage
  logWs.onclose = () => {
    logWs = null
    reconnectTimer = window.setTimeout(connectWebSocket, 3000)
  }
}

const disconnectWebSocket = (): void => {
  if (reconnectTimer) window.clearTimeout(reconnectTimer)
  reconnectTimer = undefined
  if (logWs) {
    logWs.onclose = null
    logWs.close()
    logWs = null
  }
}

const loadGroups = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/groups')
    const data = await resp.json()
    if (Array.isArray(data)) {
      groups.value = data
    }
  } catch {
    showToast?.('群列表加载失败', true)
  }
}

const buildQuery = (beforeId?: number): string => {
  const params = new URLSearchParams()
  params.set('limit', String(limit.value))
  if (groupId.value !== 'all') params.set('groupId', groupId.value)
  if (level.value !== 'all') params.set('level', level.value)
  if (keyword.value.trim()) params.set('keyword', keyword.value.trim())
  if (beforeId && beforeId > 0) params.set('beforeId', String(beforeId))
  return params.toString()
}

const applySnapshot = (data: LogQueryResult, append = false): void => {
  const list = data.entries || []
  if (append) {
    entries.value = [...list, ...entries.value]
  } else {
    entries.value = list
    void scrollToBottom()
  }
  hasMore.value = data.hasMore
  nextBeforeId.value = data.nextBeforeId || 0
  currentLevel.value = data.currentLevel || 'info'
  size.value = data.size || 0
}

const loadLogs = async (append = false): Promise<void> => {
  if (loading.value) return
  loading.value = true
  try {
    const resp = await fetch(`/admin/api/logs?${buildQuery(append ? nextBeforeId.value : undefined)}`)
    if (!resp.ok) {
      showToast?.(`日志加载失败: ${resp.status}`, true)
      return
    }
    const data: LogQueryResult = await resp.json()
    applySnapshot(data, append)
  } finally {
    loading.value = false
  }
}

const refresh = async (): Promise<void> => {
  await loadLogs(false)
  subscribe()
}

const loadOlder = async (): Promise<void> => {
  await loadLogs(true)
}

const resetFilters = async (): Promise<void> => {
  groupId.value = 'all'
  level.value = 'all'
  keyword.value = ''
  await refresh()
}

const handleWsMessage = (event: MessageEvent<string>): void => {
  const payload = JSON.parse(event.data) as { type?: string; data?: LogEntry | LogQueryResult }
  if (payload.type === 'log' && payload.data && 'content' in payload.data) {
    const entry = payload.data as LogEntry
    const lastEntry = entries.value.length > 0 ? entries.value[entries.value.length - 1] : undefined
    if (entry.id > (lastEntry?.id || 0)) {
      entries.value = [...entries.value, entry]
      if (followLatest.value) void scrollToBottom()
    }
  } else if (payload.type === 'status' && payload.data && 'size' in payload.data) {
    const snapshot = payload.data as LogQueryResult
    currentLevel.value = snapshot.currentLevel || currentLevel.value
    size.value = snapshot.size || size.value
  }
}

const onScroll = (event: Event): void => {
  const target = event.target as HTMLElement
  followLatest.value = target.scrollTop + target.clientHeight >= target.scrollHeight - 20
}

const toggleFollow = (): void => {
  followLatest.value = !followLatest.value
  if (followLatest.value) void scrollToBottom()
}

onMounted(async () => {
  await loadGroups()
  await loadLogs(false)
  connectWebSocket()
})

onUnmounted(() => disconnectWebSocket())
</script>

<template>
  <div class="log-viewer">
    <div class="page-header">
      <div class="page-title">运行日志</div>
      <div class="page-subtitle">{{ currentGroupLabel }} · 当前级别 {{ currentLevel }} · {{ size }} 条缓存</div>
    </div>

    <div class="card log-toolbar">
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">日志范围</label>
          <select v-model="groupId" class="form-input" @change="refresh">
            <option value="all">全部日志</option>
            <option value="system">系统日志</option>
            <option v-for="g in groups" :key="sessionKey(g)" :value="sessionKey(g)">
              {{ sessionLabel(g) }}
            </option>
          </select>
        </div>
        <div class="form-group">
          <label class="form-label">日志级别</label>
          <select v-model="level" class="form-input" @change="refresh">
            <option value="all">全部</option>
            <option value="trace">trace</option>
            <option value="debug">debug</option>
            <option value="info">info</option>
            <option value="warn">warn</option>
            <option value="error">error</option>
          </select>
        </div>
        <div class="form-group">
          <label class="form-label">关键字</label>
          <input v-model="keyword" class="form-input" placeholder="输入关键字后回车" @keyup.enter="refresh">
        </div>
      </div>
      <div class="log-actions">
        <button class="btn btn-secondary" type="button" @click="resetFilters">重置</button>
        <button class="btn btn-secondary" type="button" @click="refresh">刷新</button>
        <button class="btn btn-primary" type="button" @click="toggleFollow">
          {{ followLatest ? '暂停跟踪' : '继续跟踪' }}
        </button>
      </div>
    </div>

    <div class="card log-list-card">
      <div ref="logListEl" class="log-list" @scroll="onScroll">
        <div v-if="entries.length === 0" class="log-empty">暂无日志</div>
        <div v-for="entry in entries" :key="entry.id" :class="['log-line', levelClass(entry.level)]">
          <span class="log-time">{{ entry.timestamp }}</span>
          <span class="log-level">{{ entry.level }}</span>
          <span :title="entry.sessionId === '0' ? '系统' : groupTag(entry.sessionId)" class="log-group">{{
              entry.sessionId === '0' ? '系统' : groupTag(entry.sessionId)
            }}</span>
          <span class="log-source">{{ entry.source }}</span>
          <span class="log-message">{{ entry.content }}</span>
        </div>
      </div>

      <div class="log-footer">
        <button v-if="hasMore" class="btn btn-secondary" type="button" @click="loadOlder">加载更早</button>
        <span v-else class="form-hint">已显示全部可用日志</span>
        <span class="form-hint">实时跟踪 {{ followLatest ? '开启' : '关闭' }}</span>
      </div>
    </div>
  </div>
</template>
