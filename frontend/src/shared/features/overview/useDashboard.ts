import {computed, inject, onMounted, onUnmounted, reactive, ref, type Ref} from 'vue'
import type {LLMConfig, QQConfig} from '../../../vite-env'
import {useToast} from '../../../composables/useToast'
import {useAdminWebSocket} from '../../../composables/useAdminWebSocket'
import {useCoalescedRefresh} from '../../../composables/useCoalescedRefresh'
import {requestJson, requestError} from '../../../utils/api'

export function useDashboard() {


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

let storageTimer: number | undefined

const storage = ref<{databaseBytes: number | null; logBytes: number | null; timestamp: number} | null>(null)

const formatStorage = (bytes: number | null | undefined): string => {
  if (bytes == null) return '无法获取'
  return `${(bytes / 1024 ** 2).toFixed(2)} MiB`
}


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
  const data = await readData<{startTime: number; uptimeSeconds: number; storage: NonNullable<typeof storage.value>}>('/admin/api/system-info', signal)
  if (typeof data.startTime !== 'number' || typeof data.uptimeSeconds !== 'number') throw new Error('运行时间数据无效')
  startTimeEpoch.value = data.startTime
  baseUptime.value = data.uptimeSeconds
  loadMoment = Date.now()
  storage.value = data.storage
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
  storageTimer = window.setInterval(loadSystemInfo, 60000)
})


onUnmounted(() => {
  disposed = true
  for (const controller of dataRequests.values()) controller.abort()
  if (uptimeTimer) clearInterval(uptimeTimer)
  if (oneBotStatusTimer) clearInterval(oneBotStatusTimer)
  if (storageTimer) clearInterval(storageTimer)
})
return {qqConfig, showToast, dataKeys, dataState, dataRequests, disposed, loadData, readData, botRunning, botStatusSaving, oneBotStatus, loadBotStatus, loadOneBotStatus, toggleBotStatus, llmOrder, llmLabels, llmModels, llmLoading, startTimeEpoch, baseUptime, loadMoment, nowTick, uptimeTimer, oneBotStatusTimer, storageTimer, storage, formatStorage, todayStats, todayByRole, byDay, groupCount, enabledCount, totalMessages, adminCount, emojiCount, toolCount, transportLabel, oneBotAddressLabel, oneBotConnectionReady, oneBotConnectionText, fmtNum, fmtUptime, fmtDateTime, uptimeText, todayHitRate, maxDayTotal, loadLLMModels, loadSystemInfo, loadUsage, loadGroups, loadAdmins, loadEmojis, loadTools, dataSections}
}
