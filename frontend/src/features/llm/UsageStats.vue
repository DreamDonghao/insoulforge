<script lang="ts" setup>
/**
 * @file UsageStats.vue
 * @brief 用量统计 - 概览 → 角色用量 → 每日趋势 → 调用明细
 */
import {computed, inject, onMounted, onUnmounted, ref, type Ref} from 'vue'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const ws = inject<{ get: () => WebSocket | null }>('ws')

interface RoleUsage {
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

interface RecentCall {
  time: string
  role: string
  model: string
  prompt: number
  completion: number
  total: number
  cached: number
}

const loading: Ref<boolean> = ref(false)
const days: Ref<number> = ref(30)

const totalCalls: Ref<number> = ref(0)
const totalPrompt: Ref<number> = ref(0)
const totalCompletion: Ref<number> = ref(0)
const totalTokens: Ref<number> = ref(0)
const totalCached: Ref<number> = ref(0)

const byRole: Ref<RoleUsage[]> = ref([])
const byDay: Ref<DayUsage[]> = ref([])
const recent: Ref<RecentCall[]> = ref([])

// 今日统计（后端直接返回准确值）
const todayStats = ref({calls: 0, prompt: 0, completion: 0, total: 0, cached: 0})
const todayByRole: Ref<RoleUsage[]> = ref([])

const fmtLocalDate = (d: Date): string => {
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${y}-${m}-${day}`
}

const today = fmtLocalDate(new Date())

const fmtNum = (n: number): string => n.toLocaleString()
const fmtPct = (n: number): string => n.toFixed(1) + '%'

const roleLabels: Record<string, string> = {
  router: 'Router',
  executor: 'Executor',
  executorThinking: 'Executor思考',
  memory: 'Memory',
  image: 'Image'
}
const roleLabel = (role: string): string => roleLabels[role] || role || '未知'

const supportsCache = (role: string): boolean => role === 'router' || role === 'executor'

// ----- 今日数据 -----

const todayTotalCalls = computed(() => todayStats.value.calls)
const todayTotalTokens = computed(() => todayStats.value.total)
const todayPromptTotal = computed(() => todayStats.value.prompt)
const todayCompletionTotal = computed(() => todayStats.value.completion)

const todayCacheable = computed(() => todayByRole.value.filter(r => supportsCache(r.role)))
const todayCacheCached = computed(() => todayCacheable.value.reduce((s, r) => s + r.cached, 0))
const todayCachePrompt = computed(() => todayCacheable.value.reduce((s, r) => s + r.prompt, 0))
const todayHitRate = computed(() => todayCachePrompt.value > 0 ? (todayCacheCached.value / todayCachePrompt.value) * 100 : 0)

const yesterday = computed(() => fmtLocalDate(new Date(Date.now() - 86400000)))
const yesterdayDay = computed(() => byDay.value.find(d => d.day === yesterday.value))
const todayTrend = computed<'up' | 'down' | 'flat' | null>(() => {
  if (!yesterdayDay.value || todayTotalTokens.value === 0) return null
  if (todayTotalTokens.value > yesterdayDay.value.total) return 'up'
  if (todayTotalTokens.value < yesterdayDay.value.total) return 'down'
  return 'flat'
})
const trendText = computed(() => {
  if (!todayTrend.value || !yesterdayDay.value) return ''
  const diff = Math.abs(todayTotalTokens.value - yesterdayDay.value.total)
  const pct = yesterdayDay.value.total > 0 ? fmtPct((diff / yesterdayDay.value.total) * 100) : ''
  if (todayTrend.value === 'up') return `↑ 增长 ${pct}`
  if (todayTrend.value === 'down') return `↓ 下降 ${pct}`
  return '→ 持平'
})

// ----- 近 N 天数据 -----

const cacheableRoles = computed(() => byRole.value.filter(m => supportsCache(m.role)))
const monthCachePrompt = computed(() => cacheableRoles.value.reduce((s, m) => s + m.prompt, 0))
const monthCacheCached = computed(() => cacheableRoles.value.reduce((s, m) => s + m.cached, 0))
const monthHitRate = computed(() => monthCachePrompt.value > 0 ? (monthCacheCached.value / monthCachePrompt.value) * 100 : 0)

const dailyAvgCalls = computed(() => byDay.value.length > 0 ? Math.round(totalCalls.value / byDay.value.length) : 0)
const dailyAvgTokens = computed(() => byDay.value.length > 0 ? Math.round(totalTokens.value / byDay.value.length) : 0)

const roleTotal = computed(() => byRole.value.reduce((s, m) => s + m.total, 0))

// 合并今日 + 周期角色数据
interface MergedRole {
  role: string
  model: string
  todayCalls: number
  todayTokens: number
  todayCacheRate: string
  periodCalls: number
  periodTokens: number
  share: number
  avgPerCall: number
  cacheRate: string
  cached: number
  prompt: number
}

const mergedRoles = computed<MergedRole[]>(() => {
  const todayMap = new Map<string, RoleUsage>()
  for (const r of todayByRole.value) todayMap.set(r.role, r)

  return byRole.value.map(r => {
    const t = todayMap.get(r.role)
    const rate = supportsCache(r.role) && r.prompt > 0 ? fmtPct((r.cached / r.prompt) * 100) : '—'
    const todayRate = t && supportsCache(r.role) && t.prompt > 0 ? fmtPct((t.cached / t.prompt) * 100) : '—'
    return {
      role: r.role,
      model: r.model,
      todayCalls: t?.calls || 0,
      todayTokens: t?.total || 0,
      todayCacheRate: todayRate,
      periodCalls: r.calls,
      periodTokens: r.total,
      share: roleTotal.value > 0 ? (r.total / roleTotal.value) * 100 : 0,
      avgPerCall: r.calls > 0 ? Math.round(r.total / r.calls) : 0,
      cacheRate: rate,
      cached: r.cached,
      prompt: r.prompt,
    }
  })
})

// 每日趋势最大值
const maxDayTotal = computed(() => {
  if (byDay.value.length === 0) return 1
  return Math.max(...byDay.value.map(d => d.total), 1)
})

const loadUsage = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch(`/admin/api/usage?days=${days.value}`)
    if (!resp.ok) {
      showToast!('加载失败: ' + resp.status, true)
      return
    }
    const data = await resp.json()
    totalCalls.value = data.total_calls || 0
    totalPrompt.value = data.total_prompt || 0
    totalCompletion.value = data.total_completion || 0
    totalTokens.value = data.total_tokens || 0
    totalCached.value = data.total_cached || 0
    byRole.value = data.by_role || []
    byDay.value = data.by_day || []
    recent.value = data.recent || []
    todayStats.value = data.today || {calls: 0, prompt: 0, completion: 0, total: 0, cached: 0}
    todayByRole.value = data.today_by_role || []
  } catch {
    showToast!('网络错误，请检查后端服务', true)
  } finally {
    loading.value = false
  }
}

let wsMessageHandler: ((e: MessageEvent) => void) | null = null

onMounted(() => {
  loadUsage()

  const wsConn = ws?.get()
  if (wsConn) {
    wsMessageHandler = (e: MessageEvent) => {
      try {
        const msg = JSON.parse(e.data)
        if (msg.type === 'usage_updated') {
          loadUsage()
        }
      } catch { /* ignore */
      }
    }
    wsConn.addEventListener('message', wsMessageHandler)
  }
})

onUnmounted(() => {
  if (wsMessageHandler) {
    ws?.get()?.removeEventListener('message', wsMessageHandler)
    wsMessageHandler = null
  }
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">用量统计</h1>
      <p class="page-subtitle">LLM 调用量、Token 消耗与缓存命中情况</p>
    </div>

    <!-- ====== 概览卡片 ====== -->
    <div class="overview-grid">
      <!-- 今日调用 -->
      <div class="overview-card">
        <div class="ov-label">今日调用</div>
        <div class="ov-value">{{ fmtNum(todayTotalCalls) }} <span class="ov-unit">次</span></div>
        <div v-if="todayTrend" :class="'trend-' + todayTrend" class="ov-trend">{{ trendText }}</div>
        <div v-else class="ov-trend muted">暂无昨日数据</div>
      </div>

      <!-- 今日 Token -->
      <div class="overview-card">
        <div class="ov-label">今日 Token</div>
        <div class="ov-value">{{ fmtNum(todayTotalTokens) }}</div>
        <div class="ov-sub">输入 {{ fmtNum(todayPromptTotal) }} · 输出 {{ fmtNum(todayCompletionTotal) }}</div>
      </div>

      <!-- 今日缓存命中率 -->
      <div class="overview-card">
        <div class="ov-label">今日缓存命中率</div>
        <div class="ov-value">{{ fmtPct(todayHitRate) }}</div>
        <div class="ov-sub">缓存 {{ fmtNum(todayCacheCached) }} tokens</div>
      </div>

      <!-- 近 N 天汇总 -->
      <div class="overview-card">
        <div class="ov-label">近 {{ days }} 天总计</div>
        <div class="ov-value">{{ fmtNum(totalTokens) }}</div>
        <div class="ov-sub">{{ fmtNum(totalCalls) }} 次调用 · 日均 {{ fmtNum(dailyAvgTokens) }} Token · 缓存命中
          {{ fmtPct(monthHitRate) }}
        </div>
      </div>
    </div>

    <!-- ====== 角色用量 ====== -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">角色用量</h3>
        <div class="card-header-right">
          <span class="card-subtitle">今日 vs 近 {{ days }} 天</span>
          <button :disabled="loading" class="btn btn-success btn-sm" @click="loadUsage">
            {{ loading ? '加载中...' : '刷新' }}
          </button>
        </div>
      </div>

      <div v-if="mergedRoles.length" class="table-container">
        <table>
          <thead>
          <tr>
            <th>角色</th>
            <th>模型</th>
            <th class="col-num">今日调用</th>
            <th class="col-num">今日 Token</th>
            <th class="col-num">今日缓存命中率</th>
            <th class="col-num">近{{ days }}天调用</th>
            <th class="col-num">近{{ days }}天 Token</th>
            <th class="col-num">占比</th>
            <th class="col-num">平均/次</th>
            <th class="col-num">近{{ days }}天缓存命中率</th>
          </tr>
          </thead>
          <tbody>
          <tr v-for="r in mergedRoles" :key="r.role">
            <td><span class="role-name">{{ roleLabel(r.role) }}</span></td>
            <td><code>{{ r.model }}</code></td>
            <td class="col-num">{{ r.todayCalls > 0 ? fmtNum(r.todayCalls) : '—' }}</td>
            <td class="col-num">{{ r.todayTokens > 0 ? fmtNum(r.todayTokens) : '—' }}</td>
            <td class="col-num">{{ r.todayCacheRate }}</td>
            <td class="col-num">{{ fmtNum(r.periodCalls) }}</td>
            <td class="col-num">{{ fmtNum(r.periodTokens) }}</td>
            <td class="col-num">
              <div class="share-bar-cell">
                <div :style="{ width: Math.max(r.share, 2) + '%' }" class="share-bar"></div>
                <span>{{ fmtPct(r.share) }}</span>
              </div>
            </td>
            <td class="col-num">{{ fmtNum(r.avgPerCall) }}</td>
            <td class="col-num">{{ r.cacheRate }}</td>
          </tr>
          </tbody>
        </table>
      </div>
      <div v-else class="empty-state"><p>暂无数据</p></div>
    </div>

    <!-- ====== 每日趋势 ====== -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">每日趋势</h3>
        <span class="card-subtitle">近 {{ byDay.length }} 天 Token 消耗</span>
      </div>

      <div v-if="byDay.length" class="trend-chart">
        <div
            v-for="d in byDay"
            :key="d.day"
            :class="{ 'trend-row-today': d.day === today }"
            class="trend-row"
        >
          <span class="trend-date">{{ d.day.slice(5) }}</span>
          <div class="trend-bar-wrap">
            <div
                :class="{ 'trend-bar-today': d.day === today }"
                :style="{ width: (d.total / maxDayTotal) * 100 + '%' }"
                class="trend-bar"
            ></div>
          </div>
          <span class="trend-tokens">{{ fmtNum(d.total) }}</span>
          <span class="trend-calls">{{ fmtNum(d.calls) }}次</span>
        </div>
      </div>
      <div v-else class="empty-state"><p>暂无数据</p></div>
    </div>

    <!-- ====== 调用明细 ====== -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">调用明细</h3>
        <span class="card-subtitle">最近 {{ recent.length }} 条记录</span>
      </div>

      <div v-if="recent.length" class="table-container">
        <table>
          <thead>
          <tr>
            <th>时间</th>
            <th>角色</th>
            <th>模型</th>
            <th class="col-num">输入</th>
            <th class="col-num">输出</th>
            <th class="col-num">总 Token</th>
            <th class="col-num">缓存命中率</th>
          </tr>
          </thead>
          <tbody>
          <tr v-for="(r, i) in recent" :key="i">
            <td class="cell-time">{{ r.time }}</td>
            <td>{{ roleLabel(r.role) }}</td>
            <td><code>{{ r.model }}</code></td>
            <td class="col-num">{{ fmtNum(r.prompt) }}</td>
            <td class="col-num">{{ fmtNum(r.completion) }}</td>
            <td class="col-num">{{ fmtNum(r.total) }}</td>
            <td class="col-num">
              {{ supportsCache(r.role) && r.prompt > 0 ? fmtPct((r.cached / r.prompt) * 100) : '—' }}
            </td>
          </tr>
          </tbody>
        </table>
      </div>
      <div v-else class="empty-state"><p>暂无数据</p></div>
    </div>
  </div>
</template>

<style scoped>
/* ====== 概览卡片 ====== */
.overview-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 14px;
  margin-bottom: 20px;
}

.overview-card {
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px 20px;
}

.ov-label {
  font-size: 12px;
  color: var(--text-light);
  margin-bottom: 6px;
  text-transform: uppercase;
  letter-spacing: 1px;
}

.ov-value {
  font-size: 22px;
  font-weight: 700;
  color: var(--text-primary);
  line-height: 1.2;
}

.ov-unit {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
}

.ov-sub {
  font-size: 12px;
  color: var(--text-light);
  margin-top: 4px;
}

.ov-trend {
  font-size: 12px;
  font-weight: 600;
  margin-top: 4px;
}

.ov-trend.muted {
  color: var(--text-light);
  font-weight: 400;
}

.ov-trend.trend-up {
  color: var(--success);
}

.ov-trend.trend-down {
  color: var(--danger);
}

.ov-trend.trend-flat {
  color: var(--text-light);
}

/* ====== Card 通用 ====== */
.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.card-header-right {
  display: flex;
  align-items: center;
  gap: 12px;
}

.card-subtitle {
  font-size: 12px;
  color: var(--text-light);
}

/* ====== 角色名称 ====== */
.role-name {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

/* ====== 数字列右对齐 ====== */
.col-num {
  text-align: right;
  font-variant-numeric: tabular-nums;
}

/* ====== 占比条 ====== */
.share-bar-cell {
  display: flex;
  align-items: center;
  gap: 8px;
  justify-content: flex-end;
  min-width: 80px;
}

.share-bar {
  height: 5px;
  background: var(--primary);
  border-radius: 3px;
  min-width: 2px;
  flex-shrink: 0;
}

/* ====== 每日趋势 ====== */
.trend-chart {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 4px 0;
}

.trend-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 3px 0;
  border-radius: 4px;
}

.trend-row-today {
  background: var(--primary-softer);
}

.trend-date {
  font-size: 12px;
  color: var(--text-secondary);
  width: 44px;
  flex-shrink: 0;
  text-align: right;
}

.trend-bar-wrap {
  flex: 1;
  height: 18px;
  background: var(--border-soft);
  border-radius: 4px;
  overflow: hidden;
}

.trend-bar {
  height: 100%;
  background: var(--primary);
  border-radius: 4px;
  min-width: 2px;
  transition: width 0.3s ease;
  opacity: 0.7;
}

.trend-bar-today {
  opacity: 1;
  background: var(--success);
}

.trend-tokens {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
  width: 70px;
  flex-shrink: 0;
  text-align: right;
  font-variant-numeric: tabular-nums;
}

.trend-calls {
  font-size: 12px;
  color: var(--text-light);
  width: 48px;
  flex-shrink: 0;
  text-align: right;
}

/* ====== 调用明细 ====== */
.cell-time {
  font-size: 12px;
  white-space: nowrap;
  color: var(--text-secondary);
}

/* ====== 响应式 ====== */
@media (max-width: 900px) {
  .overview-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

@media (max-width: 560px) {
  .overview-grid {
    grid-template-columns: 1fr;
  }
}
</style>