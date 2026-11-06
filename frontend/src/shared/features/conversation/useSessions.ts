import {computed, inject, nextTick, onMounted, onUnmounted, reactive, ref, type Ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../../composables/useUnsavedChanges'
import {useFormValidation} from '../../../composables/useFormValidation'
import {userIdError} from '../../../utils/validation'
import {requestJson, requestError} from '../../../utils/api'
import {useAdminWebSocket} from '../../../composables/useAdminWebSocket'
import {useCoalescedRefresh} from '../../../composables/useCoalescedRefresh'
import {isPrivateSession, sessionKey, sessionLabel} from '../../../utils/session'
import type {
  ApiResponse,
  ChatMessage,
  SessionSummary,
  LongTermMemoryEntry,
  LongTermMemoryListResult,
  QQConfig,
  ScheduledTask
} from '../../../vite-env'

export function useSessions() {


const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const qqConfig = inject<QQConfig>('qqConfig')

const wsConnected = inject<Ref<boolean>>('wsConnected') as Ref<boolean>


// 会话列表
const groups: Ref<(SessionSummary & { enabled?: boolean })[]> = ref([])

// 保存请求期间收到的绝对计数，防止稍后返回的列表快照覆盖实时更新。
const messageCounts = new Map<string, number>()

const showAddSession = ref(false)

const newSessionType = ref<'group' | 'private'>('group')

const newSessionId = ref('')

const addingSession = ref(false)

const newSessionIdLabel = computed(() => newSessionType.value === 'private' ? '用户 ID' : '群号')

const sessionForm = useFormValidation(() => ({
  'new-session-id': userIdError(newSessionId.value)
}))

const openAddSession = (): void => {
  newSessionType.value = typeFilter.value === 'private' ? 'private' : 'group'
  sessionForm.resetValidation()
  showAddSession.value = true
}

const closeAddSession = (): void => {
  if (addingSession.value || !confirmDiscard(newSessionId.value !== '')) return
  showAddSession.value = false
  newSessionId.value = ''
}

const addSession = async (): Promise<void> => {
  if (addingSession.value) return
  const id = newSessionId.value
  const type = newSessionType.value
  // 按字符串提交，避免 JavaScript 数值精度丢失；后端继续校验会话 ID。
  if (!sessionForm.validateForm()) return
  addingSession.value = true
  try {
    const resp = await fetch('/admin/api/group', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(type === 'private' ? {sessionType: 'private', userId: id} : {groupId: id})
    })
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '添加失败')
    showAddSession.value = false
    newSessionId.value = ''
    searchKeyword.value = id
    typeFilter.value = type
    await loadGroups()
    showToast?.(`${type === 'private' ? '私聊' : '群聊'}已添加并启用，可在列表中调整设置`)
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '添加失败', true)
  } finally {
    addingSession.value = false
  }
}

// 会话筛选
const searchKeyword = ref('')

const typeFilter: Ref<'all' | 'group' | 'private'> = ref('all')

const filteredGroups = computed(() => {
  const keyword = searchKeyword.value.trim().toLowerCase()
  return groups.value.filter(g => {
    const matchesType = typeFilter.value === 'all' ||
        (typeFilter.value === 'private' ? isPrivateSession(g) : !isPrivateSession(g))
    return matchesType && (!keyword ||
        `${g.sessionName ?? ''} ${g.targetId ?? ''} ${sessionKey(g)}`.toLowerCase().includes(keyword))
  })
})


// 当前查看聊天记录的会话
const selectedGroup: Ref<string | null> = ref(null)

const selectedGroupName: Ref<string> = ref('')

type DisplayChatMessage = ChatMessage & {
  /** 实时推送尚未持久化时使用的前端唯一键。 */
  clientKey?: string
}


const chatRecords: Ref<DisplayChatMessage[]> = ref([])

const chatContainer: Ref<HTMLDivElement | null> = ref(null)

const chatLoading: Ref<boolean> = ref(false)

const chatError = ref('')

let nextRealtimeMessageKey = 0

let disposed = false


// 编辑状态
const editingId: Ref<number | null> = ref(null)

const editContent: Ref<string> = ref('')

const editSaving = ref(false)

let editRevision = 0

const savedEditContent = ref('')

const messageDirty = (): boolean => editingId.value !== null && editContent.value !== savedEditContent.value


// 记忆弹窗
const memoryGroupId: Ref<string | null> = ref(null)

const memoryGroupName: Ref<string> = ref('')

const groupMemory: Ref<string> = ref('')

const savedMemory = ref('')

const memoryError = ref('')

const memoryLoading: Ref<boolean> = ref(false)

const memorySaving: Ref<boolean> = ref(false)


// 压缩聊天摘要只读展示，与可编辑的会话外号分开。
type ConversationSummary = {start_time: string; end_time: string; summary: string}

const summarySession = ref<{id: string; name: string} | null>(null)

const summaries = ref<ConversationSummary[]>([])

const summariesLoading = ref(false)

const summariesError = ref('')

const closeSummaries = (): void => { summarySession.value = null }

const viewSummaries = async (id: string, name: string): Promise<void> => {
  summarySession.value = {id, name}
  const session = summarySession.value
  summaries.value = []
  summariesError.value = ''
  summariesLoading.value = true
  try {
    const resp = await fetch(`/admin/api/memory/${id}`)
    const data = await resp.json()
    if (!resp.ok) throw new Error(data.error || '加载聊天摘要失败')
    if (!disposed && summarySession.value === session)
      summaries.value = data.conversationSummaries ?? []
  } catch (error) {
    if (!disposed && summarySession.value === session)
      summariesError.value = error instanceof Error ? error.message : '加载聊天摘要失败'
  } finally {
    if (!disposed && summarySession.value === session) summariesLoading.value = false
  }
}


// 刷新失败时保留上次快照，错误状态独立于空列表。
const groupsError = ref('')

const groupsLoaded = ref(false)

const {refresh: loadGroups, loading} = useCoalescedRefresh(async signal => {
  try {
    const data = await requestJson<SessionSummary[]>('/admin/api/groups', {signal})
    if (signal.aborted) return
    if (!Array.isArray(data)) throw new Error('会话列表数据无效')
    groups.value = data.map(group => ({
      ...group,
      messageCount: Math.max(group.messageCount || 0, messageCounts.get(sessionKey(group)) || 0)
    }))
    groupsLoaded.value = true
    groupsError.value = ''
  } catch (cause) {
    if (!signal.aborted) groupsError.value = requestError(cause, '会话列表加载失败')
  }
})


// 切换启用状态
const groupSettings = {
  sendGroupCard: {route: 'card', message: '群名片设置已保存'},
  autoReply: {route: 'auto-reply', message: '自动回复设置已保存'},
  commandEnabled: {route: 'command-enabled', message: '命令解析设置已保存'},
  crossSessionSharing: {route: 'sharing', message: '跨会话共享设置已保存'}
} as const

type GroupSetting = keyof typeof groupSettings

const savingSettings = reactive(new Set<string>())

const isSettingSaving = (group: SessionSummary, setting: GroupSetting): boolean =>
    savingSettings.has(`${sessionKey(group)}:${setting}`)

const setGroupSetting = async (group: SessionSummary, enabled: boolean, setting: GroupSetting): Promise<void> => {
  const id = sessionKey(group)
  const key = `${id}:${setting}`
  const {route, message} = groupSettings[setting]
  if (savingSettings.has(key)) return
  savingSettings.add(key)
  try {
    const resp = await fetch(`/admin/api/group/${id}/${route}`, {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({[setting]: enabled})
    })
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '保存失败')
    group[setting] = enabled
    showToast?.(message)
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    savingSettings.delete(key)
  }
}


const removingGroups = reactive(new Set<string>())

const deletingRecords = reactive(new Set<number>())

const clearingSessions = reactive(new Set<string>())

const refreshingNames = ref(false)

const savingGroups = reactive(new Set<string>())

const toggleGroup = async (group: SessionSummary, enabled: boolean): Promise<void> => {
  const groupId = sessionKey(group)
  if (savingGroups.has(groupId)) return
  savingGroups.add(groupId)
  try {
    const resp = await fetch(`/admin/api/group/${encodeURIComponent(groupId)}/toggle`, {method: 'POST'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '保存失败')
    group.enabled = enabled
    showToast?.(enabled ? '已启用' : '已禁用')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    savingGroups.delete(groupId)
  }
}


// 从管理列表移除会话
const removeGroup = async (groupId: string): Promise<void> => {
  if (removingGroups.has(groupId) || !window.confirm('确定要删除该会话吗？聊天记录将保留。')) return
  removingGroups.add(groupId)
  try {
    const resp = await fetch(`/admin/api/group/${encodeURIComponent(groupId)}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      showToast!('已删除')
      await loadGroups()
    }
  } catch (cause) {
    showToast!(requestError(cause, '移除会话失败'), true)
  } finally {
    removingGroups.delete(groupId)
  }
}


// 刷新已登记会话的名称
const refreshAllGroupNames = async (): Promise<void> => {
  if (refreshingNames.value) return
  refreshingNames.value = true
  try {
    const resp = await fetch('/admin/api/groups/refresh-names', {method: 'POST'})
    const data = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      await loadGroups()
      showToast!('群名称已刷新')
    }
  } catch (cause) {
    showToast!(requestError(cause, '刷新会话名称失败'), true)
  } finally {
    refreshingNames.value = false
  }
}


// 选择会话查看聊天记录
const selectGroup = async (sessionId: string, sessionName: string): Promise<void> => {
  if (disposed || editSaving.value || !confirmDiscard(messageDirty())) return
  finishEdit()
  if (selectedGroup.value !== sessionId) {
    chatRecords.value = []
    chatError.value = ''
  }
  selectedGroup.value = sessionId
  selectedGroupName.value = sessionName
  chatLoading.value = true

  try {
    const records = await requestJson<ChatMessage[]>(`/admin/api/chat-records/${encodeURIComponent(sessionId)}?limit=200`)
    if (!Array.isArray(records)) throw new Error('聊天记录数据无效')
    if (disposed || selectedGroup.value !== sessionId) return
    chatRecords.value = records.map((record, index) =>
        record.id === undefined ? {...record, clientKey: `snapshot-${index}`} : record)

    chatError.value = ''
    // 订阅 WebSocket（字符串形式传递，后端可正确解析大数）
    send({action: 'subscribe', sessionId: sessionId})
  } catch (cause) {
    if (!disposed && selectedGroup.value === sessionId) chatError.value = requestError(cause, '聊天记录加载失败')
  } finally {
    if (!disposed && selectedGroup.value === sessionId) chatLoading.value = false
  }

  // 等待消息列表渲染完成后滚动到底部
  await nextTick()
  scrollToBottom()
}


// 返回列表
const backToList = (): void => {
  if (editSaving.value || !confirmDiscard(messageDirty())) return
  finishEdit()
  if (selectedGroup.value) send({action: 'unsubscribe', sessionId: selectedGroup.value})
  selectedGroup.value = null
  selectedGroupName.value = ''
  chatRecords.value = []
}


// 滚动到底部
const scrollToBottom = (): void => {
  if (chatContainer.value) {
    chatContainer.value.scrollTop = chatContainer.value.scrollHeight
  }
}


// 开始编辑
const startEdit = (record: DisplayChatMessage): void => {
  if (record.id === undefined || !confirmDiscard(messageDirty())) return
  editRevision++
  editingId.value = record.id
  editContent.value = record.content
  savedEditContent.value = record.content
}


// 取消编辑
const finishEdit = (): void => {
  editRevision++
  editingId.value = null
  editContent.value = ''
}


const cancelEdit = (): void => {
  if (editSaving.value || !confirmDiscard(messageDirty())) return
  finishEdit()
}


// 保存编辑
const saveEdit = async (): Promise<void> => {
  if (editSaving.value || editingId.value === null || !editContent.value.trim()) return
  const id = editingId.value
  const content = editContent.value
  const sessionId = selectedGroup.value
  const revision = editRevision
  editSaving.value = true
  try {

    const resp = await fetch(`/admin/api/chat-record/${id}`, {
      method: 'PUT',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({content})
    })
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      if (disposed) return
      const record = selectedGroup.value === sessionId ? chatRecords.value.find(r => r.id === id) : undefined
      if (record) record.content = content
      showToast!('已更新')
      if (selectedGroup.value === sessionId && editRevision === revision) {
        savedEditContent.value = content
        if (editContent.value === content) finishEdit()
      }
    }
  } catch (cause) {
    if (!disposed) showToast!(requestError(cause, '保存记录失败'), true)
  } finally {
    editSaving.value = false
  }
}


// 删除记录
const deleteRecord = async (recordId: number): Promise<void> => {
  if (deletingRecords.has(recordId) || !window.confirm('确定删除这条记录？')) return
  const sessionId = selectedGroup.value
  deletingRecords.add(recordId)
  try {

    const resp = await fetch(`/admin/api/chat-record/${recordId}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      if (selectedGroup.value === sessionId) chatRecords.value = chatRecords.value.filter(r => r.id !== recordId)
      showToast!('已删除')
    }
  } catch (cause) {
    showToast!(requestError(cause, '删除记录失败'), true)
  } finally {
    deletingRecords.delete(recordId)
  }
}


// 清空当前会话的聊天记录
const clearGroupRecords = async (): Promise<void> => {
  const sessionId = selectedGroup.value
  const sessionName = selectedGroupName.value
  if (!sessionId || clearingSessions.has(sessionId) || editSaving.value) return
  if (!window.confirm(
    '确定清空该会话的所有聊天记录？此操作不可恢复！\n' +
    '若当前有记忆总结任务正在执行，清空后收到的新消息也可能在该任务完成时被删除。这不会撤回群内消息。',
  )) return

  finishEdit()
  clearingSessions.add(sessionId)
  try {
    const resp = await fetch(`/admin/api/chat-records/${encodeURIComponent(sessionId)}/clear`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) {
      showToast!(data.error || '清空失败', true)
      return
    }
    if (selectedGroup.value === sessionId) await selectGroup(sessionId, sessionName)
    showToast!(`“${sessionName || sessionId}”的聊天记录已清空`)
    await loadGroups()
  } catch {
    showToast!('清空失败，请检查网络与后端服务', true)
  } finally {
    clearingSessions.delete(sessionId)
  }
}


// 查看记忆
const viewMemory = async (sessionId: string, sessionName: string): Promise<void> => {
  if (memoryGroupId.value !== sessionId) {
    memoryError.value = ''
    groupMemory.value = ''
    savedMemory.value = ''
  }
  memoryGroupId.value = sessionId
  memoryGroupName.value = sessionName
  memoryLoading.value = true

  try {
    const data = await requestJson<{memory: string}>(`/admin/api/memory/${encodeURIComponent(sessionId)}`)
    if (memoryGroupId.value === sessionId) {
      groupMemory.value = data.memory || ''
      savedMemory.value = groupMemory.value
      memoryError.value = ''
    }
  } catch (cause) {
    if (memoryGroupId.value === sessionId) memoryError.value = requestError(cause, '会话外号加载失败')
  } finally {
    if (memoryGroupId.value === sessionId) memoryLoading.value = false
  }
}


// 关闭记忆弹窗
const closeMemory = (): void => {
  if (memorySaving.value || !confirmDiscard(groupMemory.value !== savedMemory.value)) return
  memoryGroupId.value = null
  memoryGroupName.value = ''
  groupMemory.value = ''
}


// 保存记忆
const saveMemory = async (): Promise<void> => {
  if (!memoryGroupId.value || memorySaving.value || memoryLoading.value || memoryError.value) return
  const memory = groupMemory.value
  memorySaving.value = true
  try {
    const resp = await fetch(`/admin/api/memory/${memoryGroupId.value}`, {
      method: 'PUT',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({memory})
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      savedMemory.value = memory
      showToast!('记忆已保存')
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '保存会话外号失败'), true)
  } finally {
    memorySaving.value = false
  }
}


// 定时任务弹窗
const tasksGroupId: Ref<string | null> = ref(null)

const tasksGroupName: Ref<string> = ref('')

const tasksList: Ref<ScheduledTask[]> = ref([])

const tasksLoading: Ref<boolean> = ref(false)

const tasksError = ref('')

const cancellingTaskId: Ref<number | null> = ref(null)


const fmtTaskTime = (unixSec: number): string => {
  const d = new Date(unixSec * 1000)
  const p = (n: number): string => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}


const fmtTaskRelative = (unixSec: number): string => {
  const diff = unixSec - Math.floor(Date.now() / 1000)
  if (diff <= 0) return '即将触发'
  if (diff < 3600) return `${Math.ceil(diff / 60)} 分钟后`
  if (diff < 86400) return `${Math.floor(diff / 3600)} 小时 ${Math.floor((diff % 3600) / 60)} 分后`
  return `${Math.floor(diff / 86400)} 天后`
}


// 查看定时任务
const viewTasks = async (sessionId: string, sessionName: string): Promise<void> => {
  if (tasksGroupId.value !== sessionId) {
    tasksList.value = []
    tasksError.value = ''
  }
  tasksGroupId.value = sessionId
  tasksGroupName.value = sessionName
  tasksLoading.value = true

  try {
    const data = await requestJson<{tasks: ScheduledTask[]}>(`/admin/api/scheduled-tasks/${encodeURIComponent(sessionId)}`)
    if (!Array.isArray(data.tasks)) throw new Error('定时任务数据无效')
    if (tasksGroupId.value !== sessionId) return
    tasksList.value = data.tasks
    tasksError.value = ''
  } catch (cause) {
    if (tasksGroupId.value === sessionId) tasksError.value = requestError(cause, '定时任务加载失败')
  } finally {
    if (tasksGroupId.value === sessionId) tasksLoading.value = false
  }
}


// 关闭定时任务弹窗
const closeTasks = (): void => {
  tasksGroupId.value = null
  tasksGroupName.value = ''
  tasksList.value = []
}


// 取消定时任务
const cancelTask = async (task: ScheduledTask): Promise<void> => {
  if (!confirm(`确定取消定时任务 #${task.id}？\n触发时间：${fmtTaskTime(task.remindTime)}\n内容：${task.content}`)) {
    return
  }
  cancellingTaskId.value = task.id
  try {
    const resp = await fetch(`/admin/api/scheduled-task/${task.id}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      tasksList.value = tasksList.value.filter(t => t.id !== task.id)
      showToast!('定时任务已取消')
    } else {
      showToast!(data.error || '取消失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '取消任务失败'), true)
  } finally {
    cancellingTaskId.value = null
  }
}


// 长期记忆弹窗
const LTM_PAGE_SIZE = 20

const ltmGroupId: Ref<string | null> = ref(null)

const ltmGroupName: Ref<string> = ref('')

const ltmList: Ref<LongTermMemoryEntry[]> = ref([])

const ltmTotal: Ref<number> = ref(0)

const ltmPage: Ref<number> = ref(0)

const ltmLoading: Ref<boolean> = ref(false)

const ltmError = ref('')

let ltmRequestVersion = 0

const ltmDeletingId: Ref<number | null> = ref(null)

const ltmTotalPages = computed(() => Math.max(1, Math.ceil(ltmTotal.value / LTM_PAGE_SIZE)))


const loadLongTermMemories = async (page = ltmPage.value): Promise<void> => {
  if (!ltmGroupId.value || ltmLoading.value) return
  const id = ltmGroupId.value
  const version = ++ltmRequestVersion
  ltmLoading.value = true
  try {
    const params = new URLSearchParams({
      sessionId: ltmGroupId.value,
      limit: String(LTM_PAGE_SIZE),
      offset: String(page * LTM_PAGE_SIZE)
    })
    const data = await requestJson<LongTermMemoryListResult>(`/admin/api/long-term-memory?${params}`)
    if (ltmGroupId.value !== id || version !== ltmRequestVersion) return
    if (!Array.isArray(data.items)) throw new Error('长期记忆数据无效')
    ltmPage.value = page
    ltmError.value = ''
    ltmList.value = data.items || []
    ltmTotal.value = data.total || 0
  } catch (cause) {
    if (ltmGroupId.value === id && version === ltmRequestVersion) ltmError.value = requestError(cause, '长期记忆加载失败')
  } finally {
    if (ltmGroupId.value === id && version === ltmRequestVersion) ltmLoading.value = false
  }
}


// 查看长期记忆
const viewLongTermMemory = async (sessionId: string, sessionName: string): Promise<void> => {
  ltmError.value = ''
  ltmLoading.value = false
  ltmGroupId.value = sessionId
  ltmGroupName.value = sessionName
  ltmPage.value = 0
  ltmList.value = []
  ltmTotal.value = 0
  await loadLongTermMemories()
}


// 关闭长期记忆弹窗
const closeLongTermMemory = (): void => {
  ++ltmRequestVersion
  ltmGroupId.value = null
  ltmGroupName.value = ''
  ltmList.value = []
  ltmTotal.value = 0
}


const changeLtmPage = (delta: number): void => {
  void loadLongTermMemories(ltmPage.value + delta)
}


// 删除长期记忆
const deleteLongTermMemory = async (id: number): Promise<void> => {
  if (!confirm('确定删除这条长期记忆？删除后无法恢复')) return
  ltmDeletingId.value = id
  try {
    const resp = await fetch(`/admin/api/long-term-memory/${id}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!('长期记忆已删除')
      await loadLongTermMemories()
    } else {
      showToast!(data.error || '删除失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '删除长期记忆失败'), true)
  } finally {
    ltmDeletingId.value = null
  }
}


const promptGroup = ref<SessionSummary | null>(null)

const promptDraft = ref('')

const savedPrompt = ref('')

const promptLoading = ref(false)

const promptSaving = ref(false)

const promptError = ref('')

const promptBytes = computed(() => new TextEncoder().encode(promptDraft.value).length)

const promptForm = useFormValidation(() => ({'group-prompt': promptBytes.value > 16000 ? '附加提示词不能超过 16000 字节' : ''}))

let promptRequestVersion = 0


const openPrompt = async (group: SessionSummary): Promise<void> => {
  promptForm.resetValidation()
  const version = ++promptRequestVersion
  promptGroup.value = group
  promptDraft.value = ''
  savedPrompt.value = ''
  promptError.value = ''
  promptLoading.value = true
  try {
    const response = await fetch(`/admin/api/group/${encodeURIComponent(sessionKey(group))}/prompt`)
    if (!response.ok) throw new Error(`附加提示词加载失败（HTTP ${response.status}）`)
    const data: {additionalPrompt: string} = await response.json()
    if (typeof data.additionalPrompt !== 'string') throw new Error('附加提示词数据无效')
    if (version === promptRequestVersion) {
      promptDraft.value = data.additionalPrompt
      savedPrompt.value = data.additionalPrompt
    }
  } catch (error) {
    if (version === promptRequestVersion) promptError.value = error instanceof Error ? error.message : '加载失败'
  } finally {
    if (version === promptRequestVersion) promptLoading.value = false
  }
}


const closePrompt = (): void => {
  if (promptSaving.value || !confirmDiscard(promptDraft.value !== savedPrompt.value)) return
  ++promptRequestVersion
  promptGroup.value = null
}


const savePrompt = async (): Promise<void> => {
  if (!promptGroup.value || promptLoading.value || promptSaving.value || promptError.value || !promptForm.validateForm()) return
  const id = sessionKey(promptGroup.value)
  const additionalPrompt = promptDraft.value
  promptSaving.value = true
  try {
    const response = await fetch(`/admin/api/group/${id}/prompt`, {
      method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({additionalPrompt})
    })
    const data: ApiResponse = await response.json()
    if (!response.ok || !data.success) throw new Error(data.error || '保存失败')
    const group = groups.value.find(item => sessionKey(item) === id)
    if (group) group.hasAdditionalPrompt = additionalPrompt.length > 0
    savedPrompt.value = additionalPrompt
    showToast?.('群附加提示词已保存')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    promptSaving.value = false
  }
}


useUnsavedChanges(() =>
    messageDirty() ||
    (!!memoryGroupId.value && groupMemory.value !== savedMemory.value) ||
    (!!promptGroup.value && promptDraft.value !== savedPrompt.value) ||
    (showAddSession.value && newSessionId.value !== ''),
    () => memorySaving.value || promptSaving.value || addingSession.value || editSaving.value)


const messageKey = (message: DisplayChatMessage): string | number => message.id ?? message.clientKey ?? message.content


const {send} = useAdminWebSocket(event => {
  const data = JSON.parse(event.data)
  if (data.type === 'session_stats_updated') {
    const {sessionId, messageCount} = data.data ?? {}
    if (typeof sessionId !== 'string' || !Number.isSafeInteger(messageCount) || messageCount < 0) return
    const count = Math.max(messageCounts.get(sessionId) || 0, messageCount)
    messageCounts.set(sessionId, count)
    const group = groups.value.find(item => sessionKey(item) === sessionId)
    if (group) group.messageCount = Math.max(group.messageCount || 0, count)
  }
  if (data.type === 'new_message' && data.sessionId === selectedGroup.value) {
    chatRecords.value.push({...data.data, clientKey: `realtime-${++nextRealtimeMessageKey}`})
    nextTick(scrollToBottom)
  }
}, {
  onConnected(socket) {
    void loadGroups()
    if (selectedGroup.value) socket.send(JSON.stringify({action: 'subscribe', sessionId: selectedGroup.value}))
  },
  onDisconnected(socket) {
    if (selectedGroup.value && socket.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify({action: 'unsubscribe', sessionId: selectedGroup.value}))
  }
})


onMounted(() => {
  if (!wsConnected.value) void loadGroups()
})


onUnmounted(() => {
  disposed = true
  ++promptRequestVersion
})
return {showToast, qqConfig, wsConnected, groups, messageCounts, showAddSession, newSessionType, newSessionId, addingSession, newSessionIdLabel, sessionForm, openAddSession, closeAddSession, addSession, searchKeyword, typeFilter, filteredGroups, selectedGroup, selectedGroupName, chatRecords, chatContainer, chatLoading, chatError, nextRealtimeMessageKey, disposed, editingId, editContent, editSaving, editRevision, savedEditContent, messageDirty, memoryGroupId, memoryGroupName, groupMemory, savedMemory, memoryError, memoryLoading, memorySaving, summarySession, summaries, summariesLoading, summariesError, closeSummaries, viewSummaries, groupsError, groupsLoaded, loadGroups, loading, groupSettings, savingSettings, isSettingSaving, setGroupSetting, removingGroups, deletingRecords, clearingSessions, refreshingNames, savingGroups, toggleGroup, removeGroup, refreshAllGroupNames, selectGroup, backToList, scrollToBottom, startEdit, finishEdit, cancelEdit, saveEdit, deleteRecord, clearGroupRecords, viewMemory, closeMemory, saveMemory, tasksGroupId, tasksGroupName, tasksList, tasksLoading, tasksError, cancellingTaskId, fmtTaskTime, fmtTaskRelative, viewTasks, closeTasks, cancelTask, LTM_PAGE_SIZE, ltmGroupId, ltmGroupName, ltmList, ltmTotal, ltmPage, ltmLoading, ltmError, ltmRequestVersion, ltmDeletingId, ltmTotalPages, loadLongTermMemories, viewLongTermMemory, closeLongTermMemory, changeLtmPage, deleteLongTermMemory, promptGroup, promptDraft, savedPrompt, promptLoading, promptSaving, promptError, promptBytes, promptForm, promptRequestVersion, openPrompt, closePrompt, savePrompt, messageKey, send, sessionKey, sessionLabel, isPrivateSession}
}
