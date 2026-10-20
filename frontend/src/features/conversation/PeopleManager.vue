<script setup lang="ts">
import {computed, onMounted, ref} from 'vue'

interface PersonSummary {
  qq: string
  name: string
  affinity: number
  pending_count: number
  updated_at: string
}
interface PersonDetail extends PersonSummary {
  stable_memory: string[]
  recent_interactions: string[]
  pending_entries: {content: string; source_session: string; time: string}[]
}

const people = ref<PersonSummary[]>([])
const selected = ref('')
const detail = ref<PersonDetail | null>(null)
const search = ref('')
const loading = ref(false)
const detailLoading = ref(false)
const error = ref('')
const detailError = ref('')
let detailRequest = 0
const filtered = computed(() => {
  const query = search.value.trim().toLowerCase()
  return people.value.filter(person => `${person.qq} ${person.name}`.toLowerCase().includes(query))
})
const scoreClass = (value: number) => value > 0 ? 'positive' : value < 0 ? 'negative' : ''
const sourceLabel = (session: string) => {
  const id = BigInt(session)
  const flag = 1n << 63n
  return id & flag ? `私聊 ${id & ~flag}` : `群聊 ${session}`
}
const formatTime = (value: string) => value ? new Date(value.replace(' ', 'T') + 'Z').toLocaleString() : '暂无'

async function fetchPeopleJson(path: string) {
  const response = await fetch(path)
  if (!response.ok) {
    if (response.status === 404) throw new Error('人物查询接口不存在，请更新并重启后端服务')
    if (response.status === 401 || response.status === 403) throw new Error('登录已失效，请重新登录')
    throw new Error(`人物查询失败：HTTP ${response.status}`)
  }
  if (!response.headers.get('content-type')?.includes('application/json')) {
    throw new Error('人物查询接口未返回 JSON，请检查后端版本及反向代理配置')
  }
  const text = await response.text()
  let data
  try {
    data = JSON.parse(text)
  } catch {
    throw new Error('人物查询接口返回了无效的 JSON')
  }
  if (data?.error) throw new Error(data.error)
  return data
}

async function selectPerson(qq: string) {
  const request = ++detailRequest
  selected.value = qq
  detail.value = null
  detailLoading.value = true
  detailError.value = ''
  try {
    const data = await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(qq)}`)
    if (request === detailRequest) detail.value = data
  } catch (cause) {
    if (request === detailRequest) detailError.value = cause instanceof Error ? cause.message : '读取人物失败'
  } finally {
    if (request === detailRequest) detailLoading.value = false
  }
}

async function loadPeople() {
  loading.value = true
  error.value = ''
  try {
    const data = await fetchPeopleJson('/admin/api/people')
    if (!Array.isArray(data)) throw new Error('人物列表响应格式不正确')
    people.value = data
    const qq = data.some((person: PersonSummary) => person.qq === selected.value) ? selected.value : data[0]?.qq
    if (qq) await selectPerson(qq)
    else { selected.value = ''; detail.value = null }
  } catch (cause) {
    error.value = cause instanceof Error ? cause.message : '读取人物列表失败'
  } finally {
    loading.value = false
  }
}
onMounted(loadPeople)
</script>

<template>
  <div>
    <div class="page-header"><h1 class="page-title">人物信息</h1><p class="page-subtitle">人物记忆与全局好感度</p></div>
    <div class="people-toolbar">
      <input v-model="search" class="form-input" placeholder="搜索昵称或 QQ 号" aria-label="搜索人物">
      <span>{{ filtered.length }} / {{ people.length }} 人</span>
      <button class="btn btn-secondary" :disabled="loading" @click="loadPeople">刷新</button>
    </div>
    <p v-if="error" role="alert">{{ error }}</p>
    <p v-else-if="loading && !people.length" class="empty">加载中...</p>
    <p v-else-if="!people.length" class="empty">暂无人物记录</p>
    <div v-else class="people-layout">
      <aside class="people-list" aria-label="人物列表">
        <p v-if="!filtered.length" class="empty">没有匹配的人物</p>
        <button v-for="person in filtered" :key="person.qq" class="person-row"
                :class="{selected: selected === person.qq}" :aria-pressed="selected === person.qq"
                @click="selectPerson(person.qq)">
          <span class="person-identity"><strong>{{ person.name || '未知' }}</strong><small>{{ person.qq }} · {{ person.pending_count }} 条待整理</small></span>
          <span :class="scoreClass(person.affinity)">{{ person.affinity > 0 ? '+' : '' }}{{ person.affinity }}</span>
        </button>
      </aside>
      <section class="person-detail" aria-live="polite" :aria-busy="detailLoading">
        <p v-if="detailLoading" class="empty">加载中...</p>
        <p v-else-if="detailError" role="alert">{{ detailError }}</p>
        <template v-else-if="detail">
          <header class="person-heading">
            <div><h2>{{ detail.name || '未知' }}</h2><span>QQ {{ detail.qq }}</span></div>
            <div class="score"><span>全局好感度</span><strong :class="scoreClass(detail.affinity)">{{ detail.affinity > 0 ? '+' : '' }}{{ detail.affinity }}</strong></div>
          </header>
          <section v-for="section in [{key: 'stable', title: '固定记忆', items: detail.stable_memory}, {key: 'recent', title: '近期互动', items: detail.recent_interactions}]" :key="section.key" class="memory-section">
            <h3>{{ section.title }} <small>{{ section.items.length }}</small></h3>
            <ul v-if="section.items.length"><li v-for="(item, index) in section.items" :key="index">{{ item }}</li></ul>
            <p v-else class="muted">暂无记录</p>
          </section>
          <section class="memory-section">
            <h3>待整理记忆 <small>{{ detail.pending_entries.length }}</small></h3>
            <p v-if="!detail.pending_entries.length" class="muted">暂无记录</p>
            <article v-for="(entry, index) in detail.pending_entries" :key="index" class="memory-entry">
              <p>{{ entry.content }}</p><small>{{ formatTime(entry.time) }} · {{ sourceLabel(entry.source_session) }}</small>
            </article>
          </section>
        </template>
      </section>
    </div>
  </div>
</template>

<style scoped>
.people-toolbar { display: flex; align-items: center; gap: 16px; margin-bottom: 24px; }
.people-toolbar input { max-width: 420px; min-width: 0; flex: 1; }
.people-toolbar span, .muted, small, .person-heading span { color: var(--text-secondary); }
.people-layout { display: grid; grid-template-columns: minmax(220px, 300px) minmax(0, 1fr); border-top: 1px solid var(--border-color); }
.people-list { border-right: 1px solid var(--border-color); }
.person-row { display: flex; justify-content: space-between; align-items: center; gap: 16px; width: 100%; min-height: 76px; padding: 16px; border: 0; border-bottom: 1px solid var(--border-color); background: transparent; color: var(--text-primary); text-align: left; cursor: pointer; }
.person-row:hover, .person-row.selected { background: var(--bg-secondary); }
.person-row.selected { box-shadow: inset 3px 0 var(--primary); }
.person-identity { display: grid; gap: 6px; min-width: 0; overflow-wrap: anywhere; }
.person-detail { min-width: 0; padding: 24px 32px; }
.person-heading { display: flex; justify-content: space-between; gap: 24px; align-items: center; padding-bottom: 24px; overflow-wrap: anywhere; }
h2, h3, p { margin-top: 0; }
h2 { font-size: 22px; margin-bottom: 8px; }
h3 { font-size: 16px; margin-bottom: 16px; }
h3 small { margin-left: 8px; font-weight: normal; }
.score { display: grid; gap: 8px; text-align: right; flex-shrink: 0; }
.score strong { font-size: 26px; }
.positive { color: var(--success); } .negative { color: var(--danger); }
.memory-section { border-top: 1px solid var(--border-color); padding: 24px 0 8px; }
.memory-entry { padding: 12px 0; border-bottom: 1px solid var(--border-color); }
.memory-entry:last-child { border-bottom: 0; }
.memory-entry p, li { white-space: pre-wrap; overflow-wrap: anywhere; line-height: 1.7; margin-bottom: 8px; }
.empty { padding: 32px 16px; color: var(--text-secondary); text-align: center; }
@media (max-width: 760px) {
  .people-layout { grid-template-columns: minmax(0, 1fr); }
  .people-list { max-height: 260px; overflow-y: auto; border-right: 0; border-bottom: 1px solid var(--border-color); }
  .person-detail { padding: 24px 0; }
  .people-toolbar { flex-wrap: wrap; gap: 12px; }
}
</style>
