<script lang="ts" setup>
/**
 * @file LogViewer.vue
 * @brief 按会话、等级和关键词查询日志，并接收实时日志
 */
import {computed, inject, nextTick, onMounted, onUnmounted, ref} from 'vue'
import RequestError from '../../components/RequestError.vue'
import {requestError} from '../../utils/api'
import type {Group, LogEntry, LogQueryResult} from '../../vite-env.d'
import {privateSessionUserId, sessionKey, sessionLabel} from '../../utils/session'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const groups = ref<Group[]>([])
const entries = ref<LogEntry[]>([])
const loading = ref(false)
const loadError = ref('')
const loaded = ref(false)
const retryAppend = ref(false)
const groupsError = ref('')
const groupsLoading = ref(false)
const followLatest = ref(true)
const connectionState = ref<'connecting' | 'connected' | 'reconnecting'>('connecting')
const connectionLabel = computed(() => ({connecting: '正在连接实时日志…', connected: '实时日志已连接', reconnecting: '实时日志已断开，正在重连；数据可能过期'}[connectionState.value]))
const hasMore = ref(false)
const nextBeforeId = ref(0)
const currentLevel = ref('info')
const size = ref(0)
const groupId = ref<'all' | 'system' | string>('all')
const level = ref<'all' | string>('all')
const keyword = ref('')
const limit = ref(200)
const activeFilters = ref({groupId: 'all', level: 'all', keyword: ''})
let replacingSnapshot = false
let liveDuringSnapshot: LogEntry[] = []
const matchesFilters = (entry: LogEntry): boolean => {
  const filter = activeFilters.value
  return (filter.groupId === 'all' || entry.sessionId === (filter.groupId === 'system' ? '0' : filter.groupId)) &&
      (filter.level === 'all' || entry.level === filter.level) &&
      (!filter.keyword || entry.source.includes(filter.keyword) || entry.content.includes(filter.keyword))
}
const logListEl = ref<HTMLElement | null>(null)
let logWs: WebSocket | null = null
let reconnectTimer: number | undefined
let scrollFrame: number | undefined
let forceScroll = false
let disposed = false
let logRequest = 0
let logController: AbortController | undefined
let groupsController: AbortController | undefined

const scrollToBottom = async (force = false): Promise<void> => {
  forceScroll ||= force
  if (scrollFrame !== undefined || disposed) return
  await nextTick()
  if (scrollFrame !== undefined || disposed) return
  // 同一帧内的日志共用一次布局读取和滚动，避免逐条强制布局。
  scrollFrame = window.requestAnimationFrame(() => {
    scrollFrame = undefined
    if ((forceScroll || followLatest.value) && logListEl.value) {
      logListEl.value.scrollTop = logListEl.value.scrollHeight
    }
    forceScroll = false
  })
}

const groupLabels = computed(() => new Map(groups.value.map(g => [sessionKey(g), sessionLabel(g)])))
const groupTag = (sessionId: string): string => {
  const label = groupLabels.value.get(sessionId)
  if (label) return label
  try {
    const userId = privateSessionUserId(sessionId)
    return userId !== null ? `私聊 ${userId}` : `群 ${sessionId}`
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
    ...activeFilters.value
  }))
}

const connectWebSocket = (): void => {
  if (disposed) return
  if (logWs && logWs.readyState <= WebSocket.OPEN) return
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
  logWs = new WebSocket(`${protocol}//${location.host}/admin/logs/ws`)
  logWs.onopen = () => {
    connectionState.value = 'connected'
    subscribe()
    // 重连后补查断线期间的日志。
    if (loaded.value) void refresh()
  }
  logWs.onmessage = handleWsMessage
  logWs.onerror = () => { connectionState.value = 'reconnecting' }
  logWs.onclose = () => {
    logWs = null
    connectionState.value = 'reconnecting'
    reconnectTimer = window.setTimeout(connectWebSocket, 3000)
  }
}

const disconnectWebSocket = (): void => {
  if (reconnectTimer) window.clearTimeout(reconnectTimer)
  reconnectTimer = undefined
  if (logWs) {
    logWs.onopen = null
    logWs.onmessage = null
    logWs.onerror = null
    logWs.onclose = null
    logWs.close()
    logWs = null
  }
}

const loadGroups = async (): Promise<void> => {
  if (disposed || groupsLoading.value) return
  groupsController = new AbortController()
  groupsLoading.value = true
  try {
    const resp = await fetch('/admin/api/groups', {signal: groupsController.signal})
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
    const data = await resp.json()
    if (disposed) return
    if (!Array.isArray(data)) throw new Error('会话列表数据无效')
    groups.value = data
    groupsError.value = ''
  } catch (cause) {
    if (!disposed) groupsError.value = requestError(cause, '日志范围加载失败')
  } finally {
    if (!disposed) groupsLoading.value = false
  }
}

const buildQuery = (beforeId?: number): string => {
  const params = new URLSearchParams()
  params.set('limit', String(limit.value))
  const filter = activeFilters.value
  if (filter.groupId !== 'all') params.set('groupId', filter.groupId)
  if (filter.level !== 'all') params.set('level', filter.level)
  if (filter.keyword) params.set('keyword', filter.keyword)
  if (beforeId && beforeId > 0) params.set('beforeId', String(beforeId))
  return params.toString()
}

const applySnapshot = (data: LogQueryResult, append = false): void => {
  const list = data.entries || []
  if (append) {
    entries.value = [...list, ...entries.value]
  } else {
    entries.value = [...new Map([...list, ...liveDuringSnapshot].map(entry => [entry.id, entry])).values()].sort((a, b) => a.id - b.id)
    void scrollToBottom(true)
  }
  hasMore.value = data.hasMore
  nextBeforeId.value = data.nextBeforeId || 0
  currentLevel.value = data.currentLevel || 'info'
  size.value = data.size || 0
}

const loadLogs = async (append = false): Promise<boolean> => {
  if (disposed || (append && loading.value)) return false
  if (!append) {
    const next = {groupId: groupId.value, level: level.value, keyword: keyword.value.trim()}
    const changed = JSON.stringify(next) !== JSON.stringify(activeFilters.value)
    activeFilters.value = next
    if (changed) {
      entries.value = entries.value.filter(matchesFilters)
      hasMore.value = false
      nextBeforeId.value = 0
    }
    replacingSnapshot = true
    liveDuringSnapshot = []
    subscribe()
  }
  const request = ++logRequest
  logController?.abort()
  const controller = new AbortController()
  logController = controller
  loading.value = true
  try {
    const resp = await fetch(`/admin/api/logs?${buildQuery(append ? nextBeforeId.value : undefined)}`, {signal: controller.signal})
    if (!resp.ok) {
      throw new Error(`HTTP ${resp.status}`)
    }
    const data: LogQueryResult = await resp.json()
    if (disposed || request !== logRequest) return false
    if (!Array.isArray(data.entries)) throw new Error('日志数据无效')
    applySnapshot(data, append)
    loadError.value = ''
    loaded.value = true
    return true
  } catch (cause) {
    if (disposed || request !== logRequest || controller.signal.aborted) return false
    retryAppend.value = append
    loadError.value = requestError(cause, '日志加载失败')
    return false
  } finally {
    if (!disposed && request === logRequest) {
      replacingSnapshot = false
      liveDuringSnapshot = []
      loading.value = false
      logController = undefined
    }
  }
}

const refresh = async (): Promise<void> => {
  await loadLogs(false)
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
  if (disposed) return
  const payload = JSON.parse(event.data) as { type?: string; data?: LogEntry | LogQueryResult }
  if (payload.type === 'log' && payload.data && 'content' in payload.data) {
    const entry = payload.data as LogEntry
    if (!matchesFilters(entry)) return
    if (replacingSnapshot) liveDuringSnapshot.push(entry)
    const lastEntry = entries.value.length > 0 ? entries.value[entries.value.length - 1] : undefined
    if (entry.id > (lastEntry?.id || 0)) {
      entries.value.push(entry)
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
  void loadGroups()
  await loadLogs(false)
  connectWebSocket()
})

onUnmounted(() => {
  disposed = true
  logController?.abort()
  groupsController?.abort()
  disconnectWebSocket()
  if (scrollFrame !== undefined) window.cancelAnimationFrame(scrollFrame)
})
</script>

<template>
  <div class="log-viewer">
    <div class="page-header">
      <div class="page-title">运行日志</div>
      <div class="page-subtitle">{{ currentGroupLabel }} · 当前级别 {{ currentLevel }} · {{ size }} 条缓存</div>
    </div>

    <RequestError :message="groupsError" :busy="groupsLoading" :stale="groups.length > 0" @retry="loadGroups"/>
    <div class="card log-toolbar">
      <div class="form-row">
        <div class="form-group">
          <label for="log-scope" class="form-label">日志范围</label>
          <select id="log-scope" v-model="groupId" class="form-input" @change="refresh">
            <option value="all">全部日志</option>
            <option value="system">系统日志</option>
            <option v-for="g in groups" :key="sessionKey(g)" :value="sessionKey(g)">
              {{ sessionLabel(g) }}
            </option>
          </select>
        </div>
        <div class="form-group">
          <label for="log-level" class="form-label">日志级别</label>
          <select id="log-level" v-model="level" class="form-input" @change="refresh">
            <option value="all">全部</option>
            <option value="trace">trace</option>
            <option value="debug">debug</option>
            <option value="info">info</option>
            <option value="warn">warn</option>
            <option value="error">error</option>
          </select>
        </div>
        <div class="form-group">
          <label for="log-keyword" class="form-label">关键字</label>
          <input id="log-keyword" v-model="keyword" class="form-input" placeholder="输入关键字后回车" @keyup.enter="refresh">
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
        <RequestError :message="loadError" :busy="loading" :stale="loaded" @retry="loadLogs(retryAppend)"/>
        <p v-if="loading && !loaded" role="status">加载中…</p>
        <div v-else-if="!loadError && entries.length === 0" class="log-empty">暂无日志</div>
        <div v-for="entry in entries" :key="entry.id" v-memo="[entry, groupLabels]"
             :class="['log-line', levelClass(entry.level)]">
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
        <span v-else-if="loaded && !loadError" class="form-hint">已显示全部可用日志</span>
        <span role="status" class="form-hint">{{ connectionLabel }}</span>
        <span class="form-hint">自动滚动 {{ followLatest ? '开启' : '关闭' }}</span>
      </div>
    </div>
  </div>
</template>
