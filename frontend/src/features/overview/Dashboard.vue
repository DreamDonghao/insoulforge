<script lang="ts" setup>
/**
 * @file Dashboard.vue
 * @brief 展示机器人状态、OneBot 连接、模型配置与近期用量
 */
import {computed, inject, onMounted, onUnmounted, reactive, ref, type Ref} from 'vue'
import type {LLMConfig, QQConfig} from '../../vite-env'
import {useToast} from '../../composables/useToast'
import {useAdminWebSocket} from '../../composables/useAdminWebSocket'
import {useCoalescedRefresh} from '../../composables/useCoalescedRefresh'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import ProcessMemory from './ProcessMemory.vue'

const qqConfig = inject<QQConfig>('qqConfig')
const {showToast} = useToast()

const dataKeys = ['botStatus', 'oneBotStatus', 'models', 'system', 'groups', 'admins', 'emojis', 'tools', 'usage'] as const
type DataKey = typeof dataKeys[number]
const dataState = reactive(Object.fromEntries(dataKeys.map(key => [key, {loaded: false, loading: false, error: ''}])) as Record<DataKey, {loaded: boolean; loading: boolean; error: string}>)
const dataRequests = new Map<DataKey, AbortController>()
let disposed = false
async function loadData(key: DataKey, update: (signal: AbortSignal) => Promise<void>): Promise<void> {
  if (disposed || dataState[key].loading) return
  const controller = new AbortController()
  dataRequests.set(key, controller)
  dataState[key].loading = true
  try {
    await update(controller.signal)
    if (disposed || controller.signal.aborted) return
    dataState[key].loaded = true
    dataState[key].error = ''
  } catch (cause) {
    if (!disposed && !controller.signal.aborted) dataState[key].error = requestError(cause, '数据加载失败')
  } finally {
    dataRequests.delete(key)
    if (!disposed) dataState[key].loading = false
  }
}
async function readData<T>(url: string, signal: AbortSignal): Promise<T> {
  const data = await requestJson<T>(url, {signal})
  if (disposed || signal.aborted) throw new DOMException('Aborted', 'AbortError')
  return data
}
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

const loadBotStatus = (): Promise<void> => loadData('botStatus', async signal => {
  const data = await readData<{running: boolean}>('/admin/api/bot-status', signal)
  if (typeof data.running !== 'boolean') throw new Error('机器人状态数据无效')
  botRunning.value = data.running
})
const loadOneBotStatus = (): Promise<void> => loadData('oneBotStatus', async signal => {
  const data = await readData<OneBotStatus>('/admin/api/onebot-status', signal)
  if ((data.transport !== 'http' && data.transport !== 'websocket') || typeof data.address !== 'string') throw new Error('OneBot 状态数据无效')
  oneBotStatus.value = data
})

const toggleBotStatus = async (): Promise<void> => {
  if (botStatusSaving.value || !dataState.botStatus.loaded) return
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
const llmOrder = ['router', 'jev', 'executor', 'executorFallback', 'executorThinking', 'image', 'imageGeneration', 'memory', 'embedding'] as const
const llmLabels: Record<string, string> = {
  router: 'Router',
  jev: 'Jev',
  executor: '回复生成',
  executorFallback: '备用模型',
  executorThinking: '深度思考',
  image: '图片识别',
  imageGeneration: '图片生成',
  memory: '记忆维护',
  embedding: '向量化'
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
  await loadData('models', async signal => {
    const data = await readData<Record<string, LLMConfig>>('/admin/api/llm-configs', signal)
    if (!data || Array.isArray(data) || typeof data !== 'object') throw new Error('模型配置数据无效')
    llmModels.value = Object.fromEntries(Object.entries(data).filter(([, cfg]) => typeof cfg?.model === 'string').map(([name, cfg]) => [name, cfg.model]))
  })
  llmLoading.value = false
}
const loadSystemInfo = (): Promise<void> => loadData('system', async signal => {
  const data = await readData<{startTime: number; uptimeSeconds: number}>('/admin/api/system-info', signal)
  if (typeof data.startTime !== 'number' || typeof data.uptimeSeconds !== 'number') throw new Error('运行时间数据无效')
  startTimeEpoch.value = data.startTime
  baseUptime.value = data.uptimeSeconds
  loadMoment = Date.now()
})
const {refresh: loadUsage} = useCoalescedRefresh(async signal => {
  dataState.usage.loading = true
  try {
    const data = await readData<{today: UsageItem; today_by_role: UsageItem[]; by_day: DayUsage[]}>('/admin/api/usage?days=7', signal)
    if (!data || !Array.isArray(data.by_day)) throw new Error('用量数据无效')
    todayStats.value = data.today || {calls: 0, prompt: 0, completion: 0, total: 0, cached: 0}
    todayByRole.value = data.today_by_role || []
    byDay.value = data.by_day
    dataState.usage.loaded = true
    dataState.usage.error = ''
  } catch (cause) {
    if (!disposed && !signal.aborted) dataState.usage.error = requestError(cause, '用量加载失败')
  } finally {
    if (!disposed) dataState.usage.loading = false
  }
})
const loadGroups = (): Promise<void> => loadData('groups', async signal => {
  const data = await readData<{enabled: boolean; messageCount: number}[]>('/admin/api/groups', signal)
  if (!Array.isArray(data)) throw new Error('会话数据无效')
  groupCount.value = data.length
  enabledCount.value = data.filter(g => g.enabled).length
  totalMessages.value = data.reduce((sum, g) => sum + (g.messageCount || 0), 0)
})
const loadAdmins = (): Promise<void> => loadData('admins', async signal => {
  const data = await readData<unknown[]>('/admin/api/admins', signal)
  if (!Array.isArray(data)) throw new Error('管理员数据无效')
  adminCount.value = data.length
})
const loadEmojis = (): Promise<void> => loadData('emojis', async signal => {
  const data = await readData<unknown[]>('/admin/api/emojis', signal)
  if (!Array.isArray(data)) throw new Error('表情数据无效')
  emojiCount.value = data.length
})
const loadTools = (): Promise<void> => loadData('tools', async signal => {
  const data = await readData<unknown[]>('/admin/api/custom-tools', signal)
  if (!Array.isArray(data)) throw new Error('工具数据无效')
  toolCount.value = data.length
})
const dataSections: {key: DataKey; label: string; retry: () => Promise<void>}[] = [
  {key: 'botStatus', label: '机器人状态', retry: loadBotStatus},
  {key: 'oneBotStatus', label: 'OneBot 状态', retry: loadOneBotStatus},
  {key: 'models', label: '模型配置', retry: loadLLMModels},
  {key: 'system', label: '运行时间', retry: loadSystemInfo},
  {key: 'groups', label: '会话和消息', retry: loadGroups},
  {key: 'admins', label: '管理员', retry: loadAdmins},
  {key: 'emojis', label: '表情', retry: loadEmojis},
  {key: 'tools', label: '工具', retry: loadTools},
  {key: 'usage', label: '近期用量', retry: loadUsage}
]

useAdminWebSocket(event => {
  try {
    const msg = JSON.parse(event.data)
    if (msg.type === 'usage_updated') loadUsage()
  } catch { /* ignore */
  }
})

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
})

onUnmounted(() => {
  disposed = true
  for (const controller of dataRequests.values()) controller.abort()
  if (uptimeTimer) clearInterval(uptimeTimer)
  if (oneBotStatusTimer) clearInterval(oneBotStatusTimer)
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">首页</h1>
      <p class="page-subtitle">{{ qqConfig?.botName || 'InSoulForge' }} 运行概况</p>
    </div>

    <div v-for="section in dataSections" :key="section.key">
      <RequestError :message="dataState[section.key].error ? section.label + '：' + dataState[section.key].error : ''"
                    :busy="dataState[section.key].loading" :stale="dataState[section.key].loaded" @retry="section.retry"/>
    </div>
    <p v-if="dataKeys.some(key => dataState[key].loading)" role="status">正在更新运行概况…</p>
    <!-- 状态条 -->
    <div class="dash-status-bar">
      <div class="status-item bot-control-item">
        <span :class="dataState.botStatus.loaded && botRunning ? 'dot-green' : 'dot-gray'" class="status-dot"></span>
        <span>{{ !dataState.botStatus.loaded ? '机器人状态待获取' : botRunning ? '机器人运行中' : '机器人已暂停' }}</span>
        <button
            :class="botRunning ? 'btn-warning' : 'btn-success'"
            :disabled="botStatusSaving || !dataState.botStatus.loaded"
            class="btn btn-sm bot-toggle"
            type="button"
            @click="toggleBotStatus"
        >
          {{ botStatusSaving ? '处理中...' : (botRunning ? '暂停机器人' : '打开机器人') }}
        </button>
      </div>
      <div class="status-item">
        <span
            :class="oneBotConnectionReady ? 'dot-green' : oneBotStatus.transport === 'websocket' ? 'dot-red' : 'dot-gray'"
            class="status-dot"></span>
        <span>{{ dataState.oneBotStatus.loaded ? oneBotConnectionText : 'OneBot 状态待获取' }}</span>
      </div>
    </div>

    <ProcessMemory/>

    <!-- 模型配置跨两行，底部资源和用量共用同一行。 -->
    <div class="dash-layout">
      <!-- 左列 -->
      <div class="dash-col">
        <!-- 运行时间 -->
        <section class="overview-section" aria-label="运行时间">
          <h2 class="section-title">运行时间</h2>
          <div class="runtime-value">{{ dataState.system.loaded ? uptimeText : '—' }}</div>
          <div class="runtime-sub">启动于 {{ fmtDateTime(startTimeEpoch) }}</div>
        </section>

        <!-- OneBot 配置 -->
        <section class="overview-section" aria-label="OneBot 配置">
          <h2 class="section-title">OneBot 配置</h2>
          <div class="kv-list">
            <div class="kv-row">
              <span class="kv-label">Bot 名称</span>
              <span class="kv-value">{{ qqConfig?.botName || '—' }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">QQ 号</span>
              <code class="kv-code">{{ qqConfig?.selfQQNumber && qqConfig.selfQQNumber !== '0' ? qqConfig.selfQQNumber : '—' }}</code>
            </div>
            <div class="kv-row">
              <span class="kv-label">传输方式</span>
              <span class="kv-value">{{ dataState.oneBotStatus.loaded ? transportLabel : '—' }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">连接状态</span>
              <span :class="oneBotConnectionReady ? 'connection-ready' : 'connection-offline'" class="connection-state">
                {{ !dataState.oneBotStatus.loaded ? '状态待获取' : oneBotConnectionReady ? '已连接' : oneBotStatus.transport === 'http' ? '未配置' : '未连接' }}
              </span>
            </div>
            <div class="kv-row">
              <span class="kv-label">{{ oneBotAddressLabel }}</span>
              <code class="kv-code kv-wrap">{{ oneBotStatus.address || '—' }}</code>
            </div>
          </div>
        </section>

        <!-- 资源 -->
        <section class="overview-section" aria-label="资源">
          <h2 class="section-title">资源</h2>
          <div class="resource-grid">
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.groups.loaded ? enabledCount : '—' }}<span class="resource-total">/{{ dataState.groups.loaded ? groupCount : '—' }}</span></div>
              <div class="resource-label">会话（启用/总）</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.groups.loaded ? fmtNum(totalMessages) : '—' }}</div>
              <div class="resource-label">消息</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.admins.loaded ? adminCount : '—' }}</div>
              <div class="resource-label">管理员</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.emojis.loaded ? emojiCount : '—' }}</div>
              <div class="resource-label">表情</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.tools.loaded ? toolCount : '—' }}</div>
              <div class="resource-label">工具</div>
            </div>
          </div>
        </section>
      </div>

      <!-- 右列 -->
      <div class="dash-col">
        <!-- 模型配置 -->
        <section class="overview-section" aria-label="模型配置">
          <h2 class="section-title">模型配置</h2>
          <div class="models-list">
            <template v-if="llmLoading">
              <span class="section-muted">加载中...</span>
            </template>
            <template v-else-if="!dataState.models.loaded"><span class="section-muted">模型数据待获取</span></template>
            <template v-else-if="llmOrder.every(n => !llmModels[n])">
              <span class="section-muted">未配置</span>
            </template>
            <template v-else>
              <div v-for="name in llmOrder" :key="name" class="model-row">
                <span class="model-name">{{ llmLabels[name] }}</span>
                <code class="model-code">{{ llmModels[name] || '未配置' }}</code>
              </div>
            </template>
          </div>
        </section>

        <!-- 每日用量 -->
        <section class="overview-section" aria-label="每日用量">
          <div class="section-header">
            <h2 class="section-title">每日用量</h2>
            <span class="section-sub">今日</span>
          </div>

          <div v-if="dataState.usage.loaded" class="usage-kpis">
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

          <div v-if="dataState.usage.loaded" class="usage-chart">
            <div v-for="d in byDay" :key="d.day" :title="`${d.day}: ${fmtNum(d.total)} Token`" class="usage-col">
              <div class="usage-bar-track">
                <div :style="{ height: (d.total / maxDayTotal) * 100 + '%' }" class="usage-bar"></div>
              </div>
              <span class="usage-day">{{ d.day.slice(5) }}</span>
            </div>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* ====== 状态条 ====== */
.dash-status-bar {
  display: flex;
  gap: 16px 24px;
  padding: 0 0 20px;
  border-bottom: 1px solid var(--border);
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
  padding-right: 24px;
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
}

.dot-red {
  background: var(--danger);
}

.dot-gray {
  background: var(--text-light);
}

/* 底部资源与用量共用行轨道，使分区边界保持对齐。 */
.dash-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1.2fr);
  gap: 0;
  grid-template-rows: auto auto auto;
  align-items: stretch;
  border-top: 1px solid var(--border);
}

.dash-col {
  display: grid;
  grid-template-rows: subgrid;
  grid-row: 1 / span 3;
  gap: 0;
  min-width: 0;
  padding-right: 28px;
}

.dash-col + .dash-col {
  border-left: 1px solid var(--border);
  padding: 0 0 0 28px;
}

.dash-col:last-child > .overview-section:first-child {
  grid-row: span 2;
}

.overview-section {
  min-width: 0;
  flex: none;
  padding: 24px 0;
  display: flex;
  flex-direction: column;
}

.overview-section + .overview-section {
  border-top: 1px solid var(--border);
}

.section-header {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 16px;
}

.section-header .section-title {
  margin-bottom: 0;
}

.section-title {
  margin: 0 0 16px;
  font-weight: 600;
  font-size: 16px;
  color: var(--text-primary);
}

.section-sub {
  margin-left: auto;
  font-size: 12px;
  color: var(--text-light);
}

.section-muted {
  font-size: 13px;
  color: var(--text-light);
}

/* ====== 模型配置 ====== */
.models-list {
  display: flex;
  flex-direction: column;
  gap: 0;
}

.model-row {
  display: grid;
  grid-template-columns: 96px minmax(0, 1fr);
  align-items: center;
  gap: 10px;
  padding: 10px 0;
  border-bottom: 1px solid var(--border);
}

.model-row:last-child {
  border-bottom: 0;
}

.model-name {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-secondary);
  flex-shrink: 0;
}

.model-code {
  font-size: 12px;
  min-width: 0;
  width: fit-content;
  max-width: 100%;
  overflow-wrap: anywhere;
  white-space: normal;
  padding: 0;
  background: transparent;
}

/* ====== 每日用量 ====== */
.usage-kpis {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
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
  letter-spacing: 0;
}

.kpi-value {
  overflow-wrap: anywhere;
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
  height: 90px;
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
  border-radius: 0;
  display: flex;
  align-items: flex-end;
  overflow: hidden;
}

.usage-bar {
  width: 100%;
  background: var(--primary-strong);
  border-radius: 0;
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
  letter-spacing: 0;
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
  min-width: 0;
}

.kv-label {
  font-size: 13px;
  color: var(--text-secondary);
  white-space: nowrap;
}

.kv-value {
  min-width: 0;
  overflow-wrap: anywhere;
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
  padding: 0;
  background: transparent;
}

.kv-wrap {
  min-width: 0;
  max-width: none;
  white-space: normal;
  word-break: break-all;
  text-align: right;
}

/* ====== 资源 ====== */
.resource-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.resource-tile {
  display: flex;
  flex-direction: column;
  justify-content: center;
  padding: 8px 0;
  min-width: 0;
}

.resource-value {
  overflow-wrap: anywhere;
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
@media (max-width: 1100px) {
  .dash-layout {
    grid-template-columns: 1fr;
    grid-template-rows: auto;
  }

  .dash-col {
    display: flex;
    flex-direction: column;
    grid-row: auto;
    padding: 0;
  }

  .dash-col + .dash-col {
    border-left: 0;
    border-top: 1px solid var(--border);
    padding: 0;
  }
}

@media (max-width: 640px) {
  .resource-grid {
    grid-template-columns: repeat(2, 1fr);
  }

  .model-row {
    grid-template-columns: 80px minmax(0, 1fr);
    gap: 8px;
  }

  .bot-control-item {
    border-right: 0;
    padding-right: 0;
    flex-wrap: wrap;
  }
}
</style>
