<script setup lang="ts">
import {computed, onMounted, onUnmounted, ref} from 'vue'
import {ArrowDownWideNarrow, ArrowUpNarrowWide, Pencil, Trash2} from '@lucide/vue'
import {privateSessionUserId} from '../../utils/session'

interface PersonIdentity {
  qq: string
  name: string
  name_pending: boolean
  affinity?: number
}
interface PersonSummary extends PersonIdentity {
  pending_count: number
  updated_at: string
}
interface PersonDetail extends PersonIdentity {
  profile: string[]
  relationship: string[]
  recent_interactions: string[]
  pending_entries: {id: string; content: string; source_session: string; time: string}[]
}

const people = ref<PersonSummary[]>([])
const selected = ref('')
const detail = ref<PersonDetail | null>(null)
const search = ref('')
const loading = ref(false)
const detailLoading = ref(false)
const error = ref('')
const detailError = ref('')
const ascending = ref(false)
const affinityEnabled = ref(true)
const editing = ref(false)
const draftText = ref('')
const saving = ref(false)
const consolidating = ref(false)
const consolidationMessage = ref('')
const editError = ref('')
let nameTimer: ReturnType<typeof setTimeout> | undefined
let disposed = false
let summaryController: AbortController | undefined
let detailController: AbortController | undefined
const indexedPeople = computed(() => people.value.map(person => ({person, qq: BigInt(person.qq)})))
const sortedPeople = computed(() => {
  const direction = ascending.value ? 1 : -1
  return [...indexedPeople.value]
    .sort((a, b) => (affinityEnabled.value ? direction * ((a.person.affinity ?? 0) - (b.person.affinity ?? 0)) : 0) ||
      (a.qq < b.qq ? -1 : a.qq > b.qq ? 1 : 0))
    .map(({person}) => person)
})
const filtered = computed(() => {
  const query = search.value.trim().toLowerCase()
  return sortedPeople.value.filter(person => `${person.qq} ${person.name}`.toLowerCase().includes(query))
})
const detailName = computed(() => {
  const person = people.value.find(person => person.qq === selected.value)
  return person?.name && person.name !== '未知' ? person.name : detail.value?.name || '未知'
})
const memorySections = computed(() => detail.value ? [
  {key: 'profile', title: '人物档案', items: detail.value.profile},
  {key: 'relationship', title: '与机器人的关系', items: detail.value.relationship},
  {key: 'recent', title: '近期互动', items: detail.value.recent_interactions}
] : [])
const scoreClass = (value = 0) => value > 0 ? 'positive' : value < 0 ? 'negative' : ''
const sourceLabel = (session: string) => {
  if (session === '0') return '历史记忆迁移'
  const userId = privateSessionUserId(session)
  return userId !== null ? `私聊 ${userId}` : `群聊 ${session}`
}
const formatTime = (value: string) => value ? new Date(value.replace(' ', 'T') + 'Z').toLocaleString() : '暂无'

async function fetchPeopleJson(path: string, signal: AbortSignal, options: RequestInit = {}) {
  const response = await fetch(path, {...options, signal})
  if (!response.ok) {
    const body = await response.json().catch(() => null)
    if (typeof body?.error === 'string') throw new Error(body.error)
    if (response.status === 409) throw new Error('人物信息已变化，请重新读取后编辑；当前草稿已保留')
    if (response.status === 400) throw new Error(affinityEnabled.value ? '输入无效，请检查好感度及记忆内容' : '输入无效，请检查记忆内容')
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
  if (saving.value || consolidating.value || (editing.value && !window.confirm('放弃未保存的修改？'))) return
  editing.value = false
  editError.value = ''
  consolidationMessage.value = ''
  detailController?.abort()
  const controller = detailController = new AbortController()
  selected.value = qq
  detail.value = null
  detailLoading.value = true
  detailError.value = ''
  try {
    const data = await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(qq)}`, controller.signal)
    if (!disposed && !controller.signal.aborted) {
      affinityEnabled.value = typeof data.affinity === 'number'
      detail.value = data
    }
  } catch (cause) {
    if (!disposed && !controller.signal.aborted)
      detailError.value = cause instanceof Error ? cause.message : '读取人物失败'
  } finally {
    if (detailController === controller) detailLoading.value = false
  }
}

async function loadPeople(refreshDetail = true) {
  if (disposed || loading.value || editing.value || saving.value || consolidating.value) return
  clearTimeout(nameTimer)
  const controller = summaryController = new AbortController()
  loading.value = true
  error.value = ''
  try {
    const [data, config] = await Promise.all([
      fetchPeopleJson('/admin/api/people', controller.signal),
      fetchPeopleJson('/admin/api/memory-config', controller.signal)
    ])
    if (disposed) return
    if (!Array.isArray(data)) throw new Error('人物列表响应格式不正确')
    const changed = affinityEnabled.value !== (config.affinityEnabled ?? true)
    affinityEnabled.value = config.affinityEnabled ?? true
    people.value = data
    const qq = data.some((person: PersonSummary) => person.qq === selected.value) ? selected.value : filtered.value[0]?.qq
    if (!qq) {
      selected.value = ''
      detail.value = null
    } else if (refreshDetail || changed) {
      await selectPerson(qq)
    }
  } catch (cause) {
    if (!disposed && !controller.signal.aborted)
      error.value = cause instanceof Error ? cause.message : '读取人物列表失败'
  } finally {
    loading.value = false
    if (!disposed && !editing.value && people.value.some(person => person.name_pending))
      nameTimer = setTimeout(() => void loadPeople(false), 2000)
  }
}
function beginEdit() {
  if (!detail.value || consolidating.value) return
  clearTimeout(nameTimer)
  editing.value = true
  const {affinity, profile, relationship, recent_interactions, pending_entries} = detail.value
  draftText.value = JSON.stringify({...(affinityEnabled.value ? {affinity} : {}), profile, relationship, recent_interactions,
    pending_entries: pending_entries.map(({id, content}) => ({id, content}))}, null, 2)
  editError.value = ''
}
function cancelEdit() {
  editing.value = false
  editError.value = ''
  void loadPeople(false)
}
async function savePerson() {
  if (!editing.value || !detail.value || saving.value) return
  let value
  try {
    value = JSON.parse(draftText.value)
  } catch {
    editError.value = 'JSON 格式不正确，请检查后保存'
    return
  }
  saving.value = true
  editError.value = ''
  let configurationChanged = false
  try {
    const signal = new AbortController().signal
    const config = await fetchPeopleJson('/admin/api/memory-config', signal)
    if ((config.affinityEnabled ?? true) === affinityEnabled.value) {
      if (!affinityEnabled.value) delete value.affinity
      await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(selected.value)}`, signal, {
        method: 'PUT', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({expected: detail.value, replacement: value})
      })
    } else configurationChanged = true
    editing.value = false
    draftText.value = ''
  } catch (cause) {
    editError.value = cause instanceof Error ? cause.message : '保存失败'
  } finally { saving.value = false }
  if (!editing.value && !disposed) await loadPeople()
  if (configurationChanged && !disposed) editError.value = '好感度设置已变化，已重新读取人物信息，请重新编辑'
}
async function deletePerson() {
  if (!detail.value || saving.value || consolidating.value || !window.confirm(`删除 ${detailName.value}（${selected.value}）的全部人物记忆和好感度？聊天记录不受影响，后续聊天可重新建立档案。`)) return
  const index = filtered.value.findIndex(person => person.qq === selected.value)
  const next = filtered.value[index + 1]?.qq || filtered.value[index - 1]?.qq || ''
  saving.value = true
  editError.value = ''
  try {
    await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(selected.value)}`, new AbortController().signal, {
      method: 'DELETE', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({expected: detail.value})
    })
    selected.value = next
    detail.value = null
  } catch (cause) {
    editError.value = cause instanceof Error ? cause.message : '删除失败'
    return
  } finally { saving.value = false }
  if (!disposed) await loadPeople()
}
async function consolidatePerson() {
  if (!detail.value || editing.value || saving.value || consolidating.value) return
  clearTimeout(nameTimer)
  consolidating.value = true
  consolidationMessage.value = ''
  editError.value = ''
  try {
    const data = await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(selected.value)}/consolidate`,
      new AbortController().signal, {method: 'POST'})
    consolidating.value = false
    if (!disposed) {
      await loadPeople()
      consolidationMessage.value = data.message || '人物记忆已整理'
    }
  } catch (cause) {
    if (!disposed) editError.value = cause instanceof Error ? cause.message : '整理失败，原记忆已保留'
  } finally { consolidating.value = false }
}
onMounted(loadPeople)
onUnmounted(() => {
  disposed = true
  clearTimeout(nameTimer)
  summaryController?.abort()
  detailController?.abort()
})
</script>

<template>
  <div class="people-page">
    <div class="page-header"><h1 class="page-title">人物信息</h1><p class="page-subtitle">{{ affinityEnabled ? '人物记忆与全局好感度' : '人物记忆' }}</p></div>
    <div class="people-toolbar">
      <input v-model="search" class="form-input" placeholder="搜索昵称或 QQ 号" aria-label="搜索人物">
      <span>{{ filtered.length }} / {{ people.length }} 人</span>
      <button v-if="affinityEnabled" class="btn btn-secondary icon-button" :title="ascending ? '好感度：从低到高' : '好感度：从高到低'" :aria-label="ascending ? '好感度：从低到高' : '好感度：从高到低'" @click="ascending = !ascending"><ArrowUpNarrowWide v-if="ascending" :size="18"/><ArrowDownWideNarrow v-else :size="18"/></button>
      <button class="btn btn-secondary" :disabled="loading || editing || saving || consolidating" @click="loadPeople()">刷新</button>
    </div>
    <p v-if="error" role="alert">{{ error }}</p>
    <p v-else-if="loading && !people.length" class="empty">加载中...</p>
    <p v-else-if="!people.length" class="empty">暂无人物记录</p>
    <div v-else class="people-layout">
      <aside class="people-list" aria-label="人物列表">
        <p v-if="!filtered.length" class="empty">没有匹配的人物</p>
        <button v-for="person in filtered" :key="person.qq" class="person-row"
                :class="{selected: selected === person.qq}" :aria-pressed="selected === person.qq"
                :disabled="consolidating || saving"
                @click="selectPerson(person.qq)">
          <span class="person-identity"><strong>{{ person.name || '未知' }}</strong><small>{{ person.qq }} · {{ person.pending_count }} 条待整理</small></span>
          <span v-if="affinityEnabled" :class="scoreClass(person.affinity)">{{ (person.affinity ?? 0) > 0 ? '+' : '' }}{{ person.affinity }}</span>
        </button>
      </aside>
      <section class="person-detail" aria-live="polite" :aria-busy="detailLoading">
        <p v-if="detailLoading" class="empty">加载中...</p>
        <p v-else-if="detailError" role="alert">{{ detailError }}</p>
        <template v-else-if="detail">
          <div class="person-actions" v-if="!editing">
            <button class="btn btn-secondary" :disabled="saving || consolidating" @click="consolidatePerson">{{ consolidating ? '整理中...' : '立即整理' }}</button>
            <button class="btn btn-secondary" :disabled="saving || consolidating" @click="beginEdit"><Pencil :size="16"/>编辑</button>
            <button class="btn btn-danger icon-button" title="删除人物" aria-label="删除人物" :disabled="saving || consolidating" @click="deletePerson"><Trash2 :size="16"/></button>
          </div>
          <p v-if="consolidationMessage" role="status">{{ consolidationMessage }}</p>
          <p v-if="editError" role="alert">{{ editError }}</p>
          <form v-if="editing" class="person-editor" @submit.prevent="savePerson">
            <fieldset :disabled="saving">
              <header class="person-heading editor-heading">
                <div><h2>{{ detailName }}</h2><span>QQ {{ selected }}</span></div>
              </header>
              <textarea v-model="draftText" class="form-input person-json" aria-label="人物数据 JSON" spellcheck="false"/>
              <footer class="person-actions editor-footer"><button type="button" class="btn btn-secondary" @click="cancelEdit">取消</button><button class="btn btn-primary" type="submit">{{ saving ? '保存中...' : '保存' }}</button></footer>
            </fieldset>
          </form>
          <template v-else>
          <header class="person-heading">
            <div><h2>{{ detailName }}</h2><span>QQ {{ detail.qq }}</span></div>
            <div v-if="affinityEnabled" class="score"><span>全局好感度</span><strong :class="scoreClass(detail.affinity)">{{ (detail.affinity ?? 0) > 0 ? '+' : '' }}{{ detail.affinity }}</strong></div>
          </header>
          <section v-for="section in memorySections" :key="section.key" class="memory-section">
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
        </template>
      </section>
    </div>
  </div>
</template>

<style scoped>
.people-page { display: flex; flex-direction: column; height: calc(100dvh - 32px); min-height: 0; }
.people-page > .page-header, .people-toolbar { flex-shrink: 0; }
.person-actions { display: flex; justify-content: flex-end; gap: 12px; margin-bottom: 16px; }
.person-actions .btn { display: inline-flex; align-items: center; gap: 8px; }
.icon-button { display: inline-flex; align-items: center; justify-content: center; width: 36px; height: 36px; min-width: 36px; padding: 0; flex-shrink: 0; }
.person-editor fieldset { border: 0; padding: 0; margin: 0; min-width: 0; }
.person-json { display: block; width: 100%; min-height: 360px; height: 55vh; resize: vertical; font-family: monospace; font-size: 14px; line-height: 1.7; tab-size: 2; }
.editor-footer { padding-top: 16px; margin: 0; }
.people-toolbar { display: flex; align-items: center; gap: 16px; margin-bottom: 24px; }
.people-toolbar input { max-width: 420px; min-width: 0; flex: 1; }
.people-toolbar span, .muted, small, .person-heading span { color: var(--text-secondary); }
.people-layout { display: grid; grid-template-columns: minmax(220px, 300px) minmax(0, 1fr); flex: 1; min-height: 0; overflow: hidden; border-top: 1px solid var(--border-color); }
.people-list, .person-detail { min-height: 0; overflow-y: auto; overscroll-behavior-y: contain; scrollbar-gutter: stable; }
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
  .people-layout { grid-template-columns: minmax(0, 1fr); grid-template-rows: minmax(76px, 28%) minmax(0, 1fr); }
  .people-list { border-right: 0; border-bottom: 1px solid var(--border-color); }
  .person-detail { padding: 24px 0; }
  .editor-heading { flex-wrap: wrap; }
  .people-toolbar { flex-wrap: wrap; gap: 12px; }
}
</style>
