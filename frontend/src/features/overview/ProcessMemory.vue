<script lang="ts" setup>
import {computed, onMounted, onUnmounted, ref, shallowRef} from 'vue'

interface Sample {
  timestamp: number
  rssBytes: number | null
  peakRssBytes: number | null
  threads: number | null
}

const samples = shallowRef<Sample[]>([])
const latest = shallowRef<Sample | null>(null)
const currentFailed = ref(false)
const historyFailed = ref(false)
const loading = ref(true)
const selectedIndex = ref<number | null>(null)
const width = 1000
const height = 144
const day = 24 * 60 * 60 * 1000
let currentTimer: ReturnType<typeof setInterval> | undefined
let historyTimer: ReturnType<typeof setInterval> | undefined
const requests = new Map<'current' | 'history', AbortController>()
let disposed = false

const memory = (value: number | null | undefined): string =>
    value == null ? '—' : `${(value / 1024 ** 2).toFixed(2)} MiB`
const time = (value: number, seconds = false): string => new Date(value).toLocaleTimeString([], {
  hour: '2-digit', minute: '2-digit', ...(seconds ? {second: '2-digit'} : {})
})
const historyLatest = computed(() => samples.value[samples.value.length - 1] ?? null)
const range = computed(() => {
  const last = historyLatest.value?.timestamp ?? Date.now()
  const first = samples.value[0]?.timestamp ?? last
  if (first === last) return {start: last - 30000, end: last + 30000}
  return {start: Math.max(first, last - day), end: last}
})
const duration = computed(() => range.value.end - range.value.start)
const maximum = computed(() => Math.max(1, ...samples.value.map(sample => sample.rssBytes ?? 0)) * 1.1)
const x = (timestamp: number): number => 4 + (timestamp - range.value.start) / duration.value * (width - 8)
const y = (bytes: number): number => height - 4 - bytes / maximum.value * (height - 8)
const hasMemory = computed(() => samples.value.some(sample => sample.rssBytes !== null))
const paths = computed(() => {
  const result: string[] = []
  let path = ''
  let previousTime = 0
  for (const sample of samples.value) {
    if (sample.rssBytes === null || (previousTime && sample.timestamp - previousTime > 90_000)) {
      if (path) result.push(path)
      path = ''
    }
    if (sample.rssBytes !== null) {
      path += `${path ? ' L' : 'M'}${x(sample.timestamp)},${y(sample.rssBytes)}`
    }
    previousTime = sample.timestamp
  }
  if (path) result.push(path)
  return result
})
const selected = computed(() => selectedIndex.value === null ? null : samples.value[selectedIndex.value] ?? null)
const ticks = computed(() => [0, 0.25, 0.5, 0.75, 1].map(fraction =>
    time(range.value.start + fraction * duration.value, duration.value < 300000)))

const selectPoint = (event: PointerEvent): void => {
  const rect = (event.currentTarget as SVGSVGElement).getBoundingClientRect()
  const position = (event.clientX - rect.left) / rect.width * width
  const timestamp = range.value.start + Math.max(0, Math.min(1, (position - 4) / (width - 8))) * duration.value
  let nearest: number | null = null
  let distance = Infinity
  samples.value.forEach((sample, index) => {
    if (sample.rssBytes !== null && Math.abs(sample.timestamp - timestamp) < distance) {
      nearest = index
      distance = Math.abs(sample.timestamp - timestamp)
    }
  })
  selectedIndex.value = nearest
}

const clearSelection = (event: PointerEvent): void => {
  if (event.pointerType === 'mouse') selectedIndex.value = null
}

const load = async (kind: 'current' | 'history'): Promise<void> => {
  if (requests.has(kind) || disposed) return
  const controller = new AbortController()
  requests.set(kind, controller)
  const timeout = window.setTimeout(() => controller.abort(), 15000)
  try {
    const response = await fetch(`/admin/api/process-metrics${kind === 'current' ? '/current' : ''}`, {
      signal: controller.signal
    })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const data = await response.json()
    if (disposed) return
    if (kind === 'current') {
      latest.value = data as Sample
      currentFailed.value = false
    } else {
      samples.value = data.history as Sample[]
      selectedIndex.value = null
      historyFailed.value = false
    }
  } catch {
    if (!disposed) {
      if (kind === 'current') currentFailed.value = true
      else historyFailed.value = true
    }
  } finally {
    window.clearTimeout(timeout)
    requests.delete(kind)
    if (!disposed && kind === 'history') loading.value = false
  }
}

onMounted(() => {
  void load('current')
  void load('history')
  currentTimer = setInterval(() => void load('current'), 5000)
  historyTimer = setInterval(() => void load('history'), 60000)
})
onUnmounted(() => {
  disposed = true
  clearInterval(currentTimer)
  clearInterval(historyTimer)
  for (const controller of requests.values()) controller.abort()
})
</script>

<template>
  <section class="process-memory" aria-label="运行概况">
    <header>
      <h2>运行概况</h2>
      <div class="update-notes">
        <span v-if="currentFailed" class="stale">内存更新失败，数据可能已过期</span>
        <span v-else-if="latest" class="updated">内存 {{ time(latest.timestamp, true) }}</span>
        <slot name="metrics-note"/>
      </div>
    </header>
    <div class="memory-body">
    <dl>
      <slot name="before-metrics"/>
      <div><dt>驻留内存 RSS</dt><dd>{{ memory(latest?.rssBytes) }}</dd></div>
      <div><dt>启动以来峰值</dt><dd>{{ memory(latest?.peakRssBytes) }}</dd></div>
      <div><dt>线程数</dt><dd>{{ latest?.threads ?? '—' }}</dd></div>
      <slot name="after-metrics"/>
    </dl>
    <div class="history-panel">
    <div class="chart-heading">
      <h3>内存趋势</h3>
      <span v-if="historyFailed" class="stale">历史更新失败</span>
      <span v-if="selected">{{ time(selected.timestamp) }} · {{ memory(selected.rssBytes) }}</span>
    </div>
    <div v-if="hasMemory" class="chart">
      <div class="scale"><span>{{ memory(maximum) }}</span><span>0 MiB</span></div>
      <div class="plot">
        <svg :viewBox="`0 0 ${width} ${height}`" preserveAspectRatio="none" role="img"
             aria-label="驻留内存分钟采样曲线，最多保留最近 24 小时"
             @pointermove="selectPoint" @pointerdown="selectPoint" @pointerleave="clearSelection">
          <line v-for="level in [0, 0.5, 1]" :key="level" x1="0" :x2="width"
                :y1="level * height" :y2="level * height" class="grid-line"/>
          <path v-for="(path, index) in paths" :key="index" :d="path" class="memory-line"/>
          <circle v-if="historyLatest?.rssBytes != null" :cx="x(historyLatest.timestamp)" :cy="y(historyLatest.rssBytes)"
                  r="3" class="sample-point"/>
          <template v-if="selected?.rssBytes != null">
            <line :x1="x(selected.timestamp)" :x2="x(selected.timestamp)" y1="0" :y2="height" class="cursor-line"/>
            <circle :cx="x(selected.timestamp)" :cy="y(selected.rssBytes)" r="4" class="sample-point"/>
          </template>
        </svg>
        <div class="time-axis"><span v-for="(tick, index) in ticks" :key="index">{{ tick }}</span></div>
      </div>
    </div>
    <div v-else class="empty">{{ loading ? '加载中…' : historyFailed ? '暂时无法获取监控数据' : '暂无可用采样' }}</div>
    </div>
    </div>
  </section>
</template>

<style scoped>
.process-memory { padding: 24px 0; min-width: 0; }
header, .chart-heading { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px; }
h2, h3 { margin: 0; color: var(--text-primary); font-size: 16px; font-weight: 600; }
.updated, .chart-heading span, .scale, .time-axis { font-size: 12px; color: var(--text-secondary); }
.stale { font-size: 12px; color: var(--warning); }
.update-notes { display: flex; flex-wrap: wrap; gap: 8px 20px; align-items: center; }
.memory-body { display: flex; flex-direction: column; margin-top: 24px; }
dl { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 20px; margin: 0; }
.history-panel { min-width: 0; margin-top: 24px; }
dl :deep(dt) { font-size: 12px; font-weight: 400; color: var(--text-secondary); }
dl :deep(dd) { margin: 8px 0 0; font-size: 24px; font-weight: 600; color: var(--text-primary); overflow-wrap: anywhere; font-variant-numeric: tabular-nums; line-height: 1.3; }
.chart-heading h3 { font-size: 13px; font-weight: 500; color: var(--text-secondary); }
.chart-heading { min-height: 20px; margin-bottom: 12px; }
.chart { display: flex; gap: 12px; }
.scale { display: flex; flex-direction: column; justify-content: space-between; flex: 0 0 75px; height: 144px; }
.plot { flex: 1; min-width: 0; }
svg { display: block; width: 100%; height: 144px; overflow: visible; touch-action: pan-y; }
.grid-line { stroke: var(--border); stroke-width: 1; vector-effect: non-scaling-stroke; }
.memory-line { stroke: var(--primary); fill: none; stroke-width: 2; vector-effect: non-scaling-stroke; }
.sample-point { fill: var(--primary); }
.cursor-line { stroke: var(--text-secondary); stroke-dasharray: 3 4; vector-effect: non-scaling-stroke; }
.time-axis { display: flex; justify-content: space-between; margin-top: 8px; }
.empty { height: 164px; display: flex; align-items: center; justify-content: center; color: var(--text-secondary); font-size: 13px; }
dl :deep(small) { display: block; margin-top: 6px; font-size: 12px; color: var(--text-secondary); }
@media (max-width: 1050px) {
  dl { grid-template-columns: repeat(3, minmax(0, 1fr)); }
}
@media (max-width: 600px) {
  dl { grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
  dl :deep(dd) { font-size: 16px; }
  .scale { flex-basis: 58px; font-size: 10px; }
  .time-axis { font-size: 10px; }
}
</style>
