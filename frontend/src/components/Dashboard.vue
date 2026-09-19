<script lang="ts" setup>
/**
 * @file Dashboard.vue
 * @brief 首页仪表盘 - bento 布局，各模块按需占位，填满整页
 */
import {computed, inject, onMounted, onUnmounted, ref, type Ref} from 'vue'
import type {LLMConfig, QQConfig} from '../vite-env'
import {useToast} from '../composables/useToast'

const qqConfig = inject<QQConfig>('qqConfig')
const ws = inject<{ get: () => WebSocket | null }>('ws')
const {showToast} = useToast()

const botRunning = ref(true)
const botStatusSaving = ref(false)

interface OneBotStatus {
  transport: 'http' | 'websocket'
  address: string
  configured: boolean
  websocketConnected: boolean
}

const oneBotStatus: Ref<OneBotStatus> = ref({
  transport: 'http',
  address: '',
  configured: false,
  websocketConnected: false
})

const loadBotStatus = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/bot-status')
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
    const data = await resp.json()
    botRunning.value = data.running === true
  } catch {
    showToast('加载机器人状态失败', true)
  }
}

const loadOneBotStatus = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/onebot-status')
    if (!resp.ok) return
    const data: Partial<OneBotStatus> = await resp.json()
    if ((data.transport === 'http' || data.transport === 'websocket') && typeof data.address === 'string') {
      oneBotStatus.value = {
        transport: data.transport,
        address: data.address,
        configured: data.configured === true,
        websocketConnected: data.websocketConnected === true
      }
    }
  } catch { /* keep the last known OneBot status */
  }
}

const toggleBotStatus = async (): Promise<void> => {
  if (!botRunning.value && !window.confirm('确定打开机器人吗？')) return
  if (botRunning.value && !window.confirm('暂停后机器人将不再处理新的群消息，确定暂停吗？')) return

  botStatusSaving.value = true
  const nextRunning = !botRunning.value
  try {
    const resp = await fetch('/admin/api/bot-status', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({running: nextRunning})
    })
    const data = await resp.json()
    if (!resp.ok || data.success !== true) throw new Error(data.error || `HTTP ${resp.status}`)
    botRunning.value = data.running === true
    showToast(botRunning.value ? '机器人已打开' : '机器人已暂停')
  } catch (error) {
    showToast(error instanceof Error ? error.message : '切换机器人状态失败', true)
  } finally {
    botStatusSaving.value = false
  }
}

// ---- LLM 模型（与运行时配置一一对应）----
const llmOrder = ['router', 'executor', 'executorThinking', 'image', 'embedding'] as const
const llmLabels: Record<string, string> = {
  router: 'Router',
  executor: '回复生成',
  executorThinking: '深度思考',
  image: '图片识别',
  embedding: '向量化'
}
const roleColors: Record<string, string> = {
  router: 'var(--primary)',
  executor: 'var(--neon-cyan)',
  executorThinking: 'var(--neon-pink)',
  image: 'var(--success)',
  embedding: 'var(--warning)'
}
const llmModels: Ref<Record<string, string>> = ref({})
const llmLoading: Ref<boolean> = ref(true)

// ---- 运行时间 ----
const startTimeEpoch: Ref<number> = ref(0)
const baseUptime: Ref<number> = ref(0)
let loadMoment = Date.now()
const nowTick: Ref<number> = ref(Date.now())
let uptimeTimer: number | undefined
let oneBotStatusTimer: number | undefined

// ---- 每日用量 ----
interface UsageItem {
  role: string
  model: string
  calls: number
  prompt: number
  completion: number
  total: number
  cached: number
}

interface DayUsage {
  day: string
  calls: number
  total: number
}

const todayStats = ref({calls: 0, prompt: 0, completion: 0, total: 0, cached: 0})
const todayByRole: Ref<UsageItem[]> = ref([])
const byDay: Ref<DayUsage[]> = ref([])

// ---- 资源 ----
const groupCount: Ref<number> = ref(0)
const enabledCount: Ref<number> = ref(0)
const totalMessages: Ref<number> = ref(0)
const adminCount: Ref<number> = ref(0)
const emojiCount: Ref<number> = ref(0)
const toolCount: Ref<number> = ref(0)

const transportLabel = computed(() => oneBotStatus.value.transport === 'websocket' ? 'WebSocket' : 'HTTP')
const oneBotAddressLabel = computed(() => oneBotStatus.value.transport === 'websocket' ? 'WebSocket 地址' : 'HTTP 地址')
const oneBotConnectionReady = computed(() => oneBotStatus.value.transport === 'http'
  ? oneBotStatus.value.configured
  : oneBotStatus.value.websocketConnected)
const oneBotConnectionText = computed(() => {
  if (oneBotStatus.value.transport === 'http') {
    return oneBotStatus.value.configured ? 'OneBot HTTP 已配置' : 'OneBot HTTP 未配置'
  }
  return oneBotStatus.value.websocketConnected ? 'OneBot WebSocket 已连接' : 'OneBot WebSocket 未连接'
})

const fmtNum = (n: number): string => n.toLocaleString()

const fmtUptime = (s: number): string => {
  const days = Math.floor(s / 86400)
  const hours = Math.floor((s % 86400) / 3600)
  const mins = Math.floor((s % 3600) / 60)
  if (days > 0) return `${days}天 ${hours}小时`
  if (hours > 0) return `${hours}小时 ${mins}分`
  if (mins > 0) return `${mins}分`
  return `${s}秒`
}

const fmtDateTime = (epoch: number): string => {
  if (!epoch) return '—'
  const d = new Date(epoch * 1000)
  const p = (n: number): string => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

const uptimeText = computed(() => {
  void nowTick.value
  return fmtUptime(baseUptime.value + Math.max(0, Math.floor((Date.now() - loadMoment) / 1000)))
})

// 今日缓存命中率（仅统计 router + executor）
const todayHitRate = computed(() => {
  let prompt = 0, cached = 0
  for (const r of todayByRole.value) {
    if (r.role === 'router' || r.role === 'executor') {
      prompt += r.prompt
      cached += r.cached
    }
  }
  return prompt > 0 ? (cached / prompt) * 100 : 0
})

const maxDayTotal = computed(() => {
  if (byDay.value.length === 0) return 1
  return Math.max(...byDay.value.map(d => d.total), 1)
})

// ---- 数据加载 ----

const loadLLMModels = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/llm-configs')
    const data = await resp.json()
    llmModels.value = {}
    for (const [name, cfg] of Object.entries(data)) {
      const c = cfg as LLMConfig
      if (c.model) llmModels.value[name] = c.model
    }
  } catch { /* ignore */
  } finally {
    llmLoading.value = false
  }
}

const loadSystemInfo = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/system-info')
    const data = await resp.json()
    startTimeEpoch.value = data.startTime || 0
    baseUptime.value = data.uptimeSeconds || 0
    loadMoment = Date.now()
  } catch { /* ignore */
  }
}

const loadUsage = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/usage?days=7')
    const data = await resp.json()
    todayStats.value = data.today || {calls: 0, prompt: 0, completion: 0, total: 0, cached: 0}
    todayByRole.value = data.today_by_role || []
    byDay.value = data.by_day || []
  } catch { /* ignore */
  }
}

const loadGroups = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/groups')
    const data = await resp.json()
    if (Array.isArray(data)) {
      groupCount.value = data.length
      enabledCount.value = data.filter((g: any) => g.enabled).length
      totalMessages.value = data.reduce((sum: number, g: any) => sum + (g.messageCount || 0), 0)
    }
  } catch { /* ignore */
  }
}

const loadAdmins = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/admins')
    const data = await resp.json()
    adminCount.value = Array.isArray(data) ? data.length : 0
  } catch { /* ignore */
  }
}

const loadEmojis = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/emojis')
    const data = await resp.json()
    emojiCount.value = Array.isArray(data) ? data.length : 0
  } catch { /* ignore */
  }
}

const loadTools = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/custom-tools')
    const data = await resp.json()
    toolCount.value = Array.isArray(data) ? data.length : 0
  } catch { /* ignore */
  }
}

let wsMessageHandler: ((e: MessageEvent) => void) | null = null

onMounted(() => {
  loadBotStatus()
  loadOneBotStatus()
  loadLLMModels()
  loadSystemInfo()
  loadUsage()
  loadGroups()
  loadAdmins()
  loadEmojis()
  loadTools()

  uptimeTimer = window.setInterval(() => {
    nowTick.value = Date.now()
  }, 1000)
  oneBotStatusTimer = window.setInterval(loadOneBotStatus, 5000)

  const wsConn = ws?.get()
  if (wsConn) {
    wsMessageHandler = (e: MessageEvent) => {
      try {
        const msg = JSON.parse(e.data)
        if (msg.type === 'usage_updated') loadUsage()
      } catch { /* ignore */
      }
    }
    wsConn.addEventListener('message', wsMessageHandler)
  }
})

onUnmounted(() => {
  if (uptimeTimer) clearInterval(uptimeTimer)
  if (oneBotStatusTimer) clearInterval(oneBotStatusTimer)
  if (wsMessageHandler) {
    ws?.get()?.removeEventListener('message', wsMessageHandler)
    wsMessageHandler = null
  }
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">首页</h1>
      <p class="page-subtitle">{{ qqConfig?.botName || 'InSoulForge' }} 运行概况</p>
    </div>

    <!-- 状态条 -->
    <div class="dash-status-bar">
      <div class="status-item bot-control-item">
        <span :class="botRunning ? 'dot-green' : 'dot-gray'" class="status-dot"></span>
        <span>{{ botRunning ? '机器人运行中' : '机器人已暂停' }}</span>
        <button
            :class="botRunning ? 'btn-warning' : 'btn-success'"
            :disabled="botStatusSaving"
            class="btn btn-sm bot-toggle"
            type="button"
            @click="toggleBotStatus"
        >
          {{ botStatusSaving ? '处理中...' : (botRunning ? '暂停机器人' : '打开机器人') }}
        </button>
      </div>
      <div class="status-item">
        <span :class="oneBotConnectionReady ? 'dot-green' : oneBotStatus.transport === 'websocket' ? 'dot-red' : 'dot-gray'"
              class="status-dot"></span>
        <span>{{ oneBotConnectionText }}</span>
      </div>
    </div>

    <!-- 仪表盘布局：左 3 右 2 -->
    <div class="dash-layout">
      <!-- 左列 -->
      <div class="dash-col">
        <!-- 运行时间 -->
        <div class="bento-card">
          <div class="bento-header">
            <svg class="bento-icon" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <circle cx="12" cy="12" r="10"/>
              <polyline points="12 6 12 12 16 14"/>
            </svg>
            <span class="bento-title">运行时间</span>
          </div>
          <div class="runtime-value">{{ uptimeText }}</div>
          <div class="runtime-sub">启动于 {{ fmtDateTime(startTimeEpoch) }}</div>
        </div>

        <!-- OneBot 配置 -->
        <div class="bento-card">
          <div class="bento-header">
            <svg class="bento-icon" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path
                  d="M8 12h.01M12 12h.01M16 12h.01M21 12c0 4.418-4.03 8-9 8a9.863 9.863 0 01-4.255-.949L3 20l1.395-3.72C3.512 15.042 3 13.574 3 12c0-4.418 4.03-8 9-8s9 3.582 9 8z"/>
            </svg>
            <span class="bento-title">OneBot 配置</span>
          </div>
          <div class="kv-list">
            <div class="kv-row">
              <span class="kv-label">Bot 名称</span>
              <span class="kv-value">{{ qqConfig?.botName || '—' }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">QQ 号</span>
              <code class="kv-code">{{ qqConfig?.selfQQNumber || '—' }}</code>
            </div>
            <div class="kv-row">
              <span class="kv-label">传输方式</span>
              <span class="kv-value">{{ transportLabel }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">连接状态</span>
              <span :class="oneBotConnectionReady ? 'connection-ready' : 'connection-offline'" class="connection-state">
                {{ oneBotConnectionReady ? '已连接' : oneBotStatus.transport === 'http' ? '未配置' : '未连接' }}
              </span>
            </div>
            <div class="kv-row">
              <span class="kv-label">{{ oneBotAddressLabel }}</span>
              <code class="kv-code kv-wrap">{{ oneBotStatus.address || '—' }}</code>
            </div>
          </div>
        </div>

        <!-- 资源 -->
        <div class="bento-card">
          <div class="bento-header">
            <svg class="bento-icon" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/>
              <circle cx="9" cy="7" r="4"/>
            </svg>
            <span class="bento-title">资源</span>
          </div>
          <div class="resource-grid">
            <div class="resource-tile">
              <div class="resource-value">{{ enabledCount }}<span class="resource-total">/{{ groupCount }}</span></div>
              <div class="resource-label">群聊（启用/总）</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ fmtNum(totalMessages) }}</div>
              <div class="resource-label">消息</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ adminCount }}</div>
              <div class="resource-label">管理员</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ emojiCount }}</div>
              <div class="resource-label">表情</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ toolCount }}</div>
              <div class="resource-label">工具</div>
            </div>
          </div>
        </div>
      </div>

      <!-- 右列 -->
      <div class="dash-col">
        <!-- 模型配置 -->
        <div class="bento-card">
          <div class="bento-header">
            <svg class="bento-icon" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/>
            </svg>
            <span class="bento-title">模型配置</span>
          </div>
          <div class="models-list">
            <template v-if="llmLoading">
              <span class="bento-muted">加载中...</span>
            </template>
            <template v-else-if="llmOrder.every(n => !llmModels[n])">
              <span class="bento-muted">未配置</span>
            </template>
            <template v-else>
              <div v-for="name in llmOrder" :key="name" class="model-row">
                <span :style="{ background: roleColors[name] }" class="model-dot"></span>
                <span class="model-name">{{ llmLabels[name] }}</span>
                <code class="model-code">{{ llmModels[name] || '未配置' }}</code>
              </div>
            </template>
          </div>
        </div>

        <!-- 每日用量 -->
        <div class="bento-card">
          <div class="bento-header">
            <svg class="bento-icon" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2zm0 18a8 8 0 1 1 8-8 8 8 0 0 1-8 8z"/>
              <path d="M12 6v6l4 2"/>
            </svg>
            <span class="bento-title">每日用量</span>
            <span class="bento-sub">今日</span>
          </div>

          <div class="usage-kpis">
            <div class="usage-kpi">
              <div class="kpi-label">调用</div>
              <div class="kpi-value">{{ fmtNum(todayStats.calls) }} <span class="kpi-unit">次</span></div>
            </div>
            <div class="usage-kpi">
              <div class="kpi-label">Token</div>
              <div class="kpi-value">{{ fmtNum(todayStats.total) }}</div>
            </div>
            <div class="usage-kpi">
              <div class="kpi-label">缓存命中率</div>
              <div class="kpi-value">{{ todayHitRate.toFixed(1) }}%</div>
            </div>
          </div>

          <div class="usage-chart">
            <div v-for="d in byDay" :key="d.day" :title="`${d.day}: ${fmtNum(d.total)} Token`" class="usage-col">
              <div class="usage-bar-track">
                <div :style="{ height: (d.total / maxDayTotal) * 100 + '%' }" class="usage-bar"></div>
              </div>
              <span class="usage-day">{{ d.day.slice(5) }}</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* ====== 状态条 ====== */
.dash-status-bar {
  display: flex;
  gap: 24px;
  margin-bottom: 16px;
  padding: 12px 18px;
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-radius: 10px;
  flex-wrap: wrap;
}

.status-item {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--text-secondary);
}

.bot-control-item {
  padding-right: 8px;
  border-right: 1px solid var(--border);
}

.bot-toggle {
  margin-left: 4px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot-green {
  background: var(--success);
  box-shadow: 0 0 6px var(--success);
}

.dot-red {
  background: var(--danger);
  box-shadow: 0 0 6px var(--danger);
}

.dot-gray {
  background: var(--text-light);
}

/* ====== 仪表盘布局（左 3 右 2） ====== */
.dash-layout {
  display: grid;
  grid-template-columns: 1fr 1.2fr;
  gap: 16px;
  align-items: stretch;
  min-height: calc(100vh - 220px);
}

.dash-col {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.bento-card {
  flex: 1;
  min-height: 0;
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 18px 20px;
  display: flex;
  flex-direction: column;
  transition: border-color 0.15s ease, box-shadow 0.15s ease;
}

.bento-card:hover {
  border-color: var(--neon-cyan);
  box-shadow: 0 0 14px var(--neon-cyan-glow);
}

/* ====== 卡片头部 ====== */
.bento-header {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 14px;
}

.bento-icon {
  width: 18px;
  height: 18px;
  flex-shrink: 0;
  color: var(--primary);
  opacity: 0.8;
}

.bento-title {
  font-weight: 600;
  font-size: 14px;
  color: var(--text-primary);
}

.bento-sub {
  margin-left: auto;
  font-size: 12px;
  color: var(--text-light);
}

.bento-muted {
  font-size: 13px;
  color: var(--text-light);
}

/* ====== 模型配置 ====== */
.models-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.model-row {
  display: flex;
  align-items: center;
  gap: 10px;
}

.model-dot {
  width: 9px;
  height: 9px;
  border-radius: 50%;
  flex-shrink: 0;
  box-shadow: 0 0 8px currentColor;
}

.model-name {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-secondary);
  width: 96px;
  flex-shrink: 0;
}

.model-code {
  font-size: 12px;
  max-width: none;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* ====== 每日用量 ====== */
.usage-kpis {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 12px;
  margin-bottom: 16px;
}

.usage-kpi {
  display: flex;
  flex-direction: column;
}

.kpi-label {
  font-size: 12px;
  color: var(--text-light);
  text-transform: uppercase;
  letter-spacing: 1px;
}

.kpi-value {
  font-size: 20px;
  font-weight: 700;
  color: var(--text-primary);
  line-height: 1.25;
  font-variant-numeric: tabular-nums;
}

.kpi-unit {
  font-size: 13px;
  font-weight: 500;
  color: var(--text-secondary);
}

.usage-chart {
  display: flex;
  align-items: flex-end;
  gap: 8px;
  flex: 1;
  min-height: 90px;
}

.usage-col {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  height: 100%;
  justify-content: flex-end;
}

.usage-bar-track {
  width: 100%;
  max-width: 28px;
  height: 70px;
  background: var(--border-soft, var(--bg-secondary));
  border-radius: 6px;
  display: flex;
  align-items: flex-end;
  overflow: hidden;
}

.usage-bar {
  width: 100%;
  background: linear-gradient(180deg, var(--primary), var(--neon-cyan));
  border-radius: 6px;
  min-height: 2px;
  transition: height 0.3s ease;
}

.usage-day {
  font-size: 10px;
  color: var(--text-light);
  white-space: nowrap;
}

/* ====== 运行时间 ====== */
.runtime-value {
  font-size: 28px;
  font-weight: 700;
  color: var(--text-primary);
  letter-spacing: -0.5px;
  font-variant-numeric: tabular-nums;
}

.runtime-sub {
  margin-top: 6px;
  font-size: 12px;
  color: var(--text-light);
}

/* ====== kv 列表（OneBot） ====== */
.kv-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.kv-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}

.kv-label {
  font-size: 13px;
  color: var(--text-secondary);
  white-space: nowrap;
}

.kv-value {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.connection-state {
  font-size: 13px;
  font-weight: 600;
}

.connection-ready {
  color: var(--success);
}

.connection-offline {
  color: var(--text-secondary);
}

.kv-code {
  font-size: 12px;
  max-width: 160px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.kv-wrap {
  max-width: none;
  white-space: normal;
  word-break: break-all;
  text-align: right;
}

/* ====== 资源 ====== */
.resource-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
  flex: 1;
}

.resource-tile {
  display: flex;
  flex-direction: column;
  justify-content: center;
  padding: 8px 10px;
  border-radius: 8px;
  background: var(--primary-softer);
}

.resource-value {
  font-size: 20px;
  font-weight: 700;
  color: var(--text-primary);
  font-variant-numeric: tabular-nums;
}

.resource-total {
  font-size: 13px;
  font-weight: 500;
  color: var(--text-light);
}

.resource-label {
  font-size: 11px;
  color: var(--text-light);
  margin-top: 2px;
}

/* ====== 响应式 ====== */
@media (max-width: 640px) {
  .dash-layout {
    grid-template-columns: 1fr;
    min-height: 0;
  }

  .usage-kpis {
    grid-template-columns: 1fr;
  }

  .resource-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}
</style>
