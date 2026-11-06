import {inject, computed, nextTick, onMounted, onUnmounted, ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../../composables/useUnsavedChanges'
import {sessionIdLabel} from '../../../utils/session'

export function usePeople() {


interface PersonIdentity {
  user_id: string
  name: string
  estimated_tokens: number
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

const sortBy = ref<'affinity' | 'tokens'>('affinity')

const sortLabel = computed(() => sortBy.value === 'tokens' || !affinityEnabled.value ? '预估 Token' : '好感度')

const affinityEnabled = ref(true)

const editing = ref(false)

const draftText = ref('')

const saving = ref(false)

const savedDraft = ref('')

const isDirty = (): boolean => editing.value && draftText.value !== savedDraft.value

const consolidating = ref(false)

const compressing = ref(false)

const consolidationMessage = ref('')

useUnsavedChanges(isDirty, () => saving.value || consolidating.value)

const editError = ref('')

const jsonInvalid = ref(false)

const jsonEditor = ref<HTMLTextAreaElement | null>(null)

const focusJsonEditor = (): void => { void nextTick(() => jsonEditor.value?.focus()) }

let nameTimer: ReturnType<typeof setTimeout> | undefined

let disposed = false

let summaryController: AbortController | undefined

let detailController: AbortController | undefined

const sortedPeople = computed(() => {
  const direction = ascending.value ? 1 : -1
  return [...people.value]
    .sort((a, b) => direction * (sortBy.value === 'tokens' || !affinityEnabled.value
      ? (a.estimated_tokens ?? 0) - (b.estimated_tokens ?? 0)
      : (a.affinity ?? 0) - (b.affinity ?? 0)) ||
      (a.user_id < b.user_id ? -1 : a.user_id > b.user_id ? 1 : 0))
})

const filtered = computed(() => {
  const query = search.value.trim().toLowerCase()
  return sortedPeople.value.filter(person => `${person.user_id} ${person.name}`.toLowerCase().includes(query))
})

const detailName = computed(() => {
  const person = people.value.find(person => person.user_id === selected.value)
  return person?.name && person.name !== '未知' ? person.name : detail.value?.name || '未知'
})

const memorySections = computed(() => detail.value ? [
  {key: 'profile', title: '人物档案', items: detail.value.profile},
  {key: 'relationship', title: '与机器人的关系', items: detail.value.relationship},
  {key: 'recent', title: '近期互动', items: detail.value.recent_interactions}
] : [])

const scoreClass = (value = 0) => value > 0 ? 'positive' : value < 0 ? 'negative' : ''

const sourceLabel = (session: string) => {
  return sessionIdLabel(session)
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


async function selectPerson(user_id: string) {
  if (saving.value || consolidating.value || !confirmDiscard(isDirty())) return
  editing.value = false
  editError.value = ''
  consolidationMessage.value = ''
  detailController?.abort()
  const controller = detailController = new AbortController()
  selected.value = user_id
  detail.value = null
  detailLoading.value = true
  detailError.value = ''
  try {
    const data = await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(user_id)}`, controller.signal)
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
    const user_id = data.some((person: PersonSummary) => person.user_id === selected.value) ? selected.value : filtered.value[0]?.user_id
    if (!user_id) {
      selected.value = ''
      detail.value = null
    } else if (refreshDetail || changed) {
      await selectPerson(user_id)
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
  jsonInvalid.value = false
  if (!detail.value || consolidating.value) return
  clearTimeout(nameTimer)
  editing.value = true
  const {affinity, profile, relationship, recent_interactions, pending_entries} = detail.value
  draftText.value = JSON.stringify({...(affinityEnabled.value ? {affinity} : {}), profile, relationship, recent_interactions,
    pending_entries: pending_entries.map(({id, content}) => ({id, content}))}, null, 2)
  savedDraft.value = draftText.value
  editError.value = ''
  focusJsonEditor()
}

function cancelEdit() {
  if (saving.value || !confirmDiscard(isDirty())) return
  editing.value = false
  editError.value = ''
  void loadPeople(false)
}

async function savePerson() {
  if (!editing.value || !detail.value || saving.value) return
  let value
  jsonInvalid.value = false
  try {
    value = JSON.parse(draftText.value)
  } catch {
    editError.value = 'JSON 格式不正确，请检查后保存'
    jsonInvalid.value = true
    focusJsonEditor()
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
  if (editing.value && editError.value && !disposed) focusJsonEditor()
  if (!editing.value && !disposed) await loadPeople()
  if (configurationChanged && !disposed) editError.value = '好感度设置已变化，已重新读取人物信息，请重新编辑'
}

async function deletePerson() {
  if (!detail.value || saving.value || consolidating.value || !window.confirm(`删除 ${detailName.value}（${selected.value}）的全部人物记忆和好感度？聊天记录不受影响，后续聊天可重新建立档案。`)) return
  const index = filtered.value.findIndex(person => person.user_id === selected.value)
  const next = filtered.value[index + 1]?.user_id || filtered.value[index - 1]?.user_id || ''
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

async function consolidatePerson(compress = false) {
  if (!detail.value || editing.value || saving.value || consolidating.value) return
  clearTimeout(nameTimer)
  consolidating.value = true
  compressing.value = compress
  consolidationMessage.value = ''
  editError.value = ''
  try {
    const data = await fetchPeopleJson(`/admin/api/people/${encodeURIComponent(selected.value)}/${compress ? 'compress' : 'consolidate'}`,
      new AbortController().signal, {method: 'POST'})
    consolidating.value = false
    if (!disposed) {
      await loadPeople()
      consolidationMessage.value = data.message || (compress ? '人物记忆已压缩' : '人物记忆已整理')
    }
  } catch (cause) {
    if (!disposed) editError.value = cause instanceof Error ? cause.message : '处理失败，原记忆已保留'
  } finally { consolidating.value = false }
}

const mobileCache = inject<Map<string, Record<string, unknown>> | null>('mobileViewCache', null)
const preferences = {search, ascending, sortBy, selected}
const cachedPreferences = mobileCache?.get('people')
for (const [key, field] of Object.entries(preferences)) { if (cachedPreferences && key in cachedPreferences) (field as {value: unknown}).value = cachedPreferences[key] }
onUnmounted(() => mobileCache?.set('people', Object.fromEntries(Object.entries(preferences).map(([key, field]) => [key, field.value]))))
onMounted(loadPeople)

onUnmounted(() => {
  disposed = true
  clearTimeout(nameTimer)
  summaryController?.abort()
  detailController?.abort()
})
return {people, selected, detail, search, loading, detailLoading, error, detailError, ascending, sortBy, sortLabel, affinityEnabled, editing, draftText, saving, savedDraft, isDirty, consolidating, compressing, consolidationMessage, editError, jsonInvalid, jsonEditor, focusJsonEditor, nameTimer, disposed, summaryController, detailController, sortedPeople, filtered, detailName, memorySections, scoreClass, sourceLabel, formatTime, fetchPeopleJson, selectPerson, loadPeople, beginEdit, cancelEdit, savePerson, deletePerson, consolidatePerson}
}
