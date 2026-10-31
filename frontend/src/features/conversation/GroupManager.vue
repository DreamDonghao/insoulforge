<script lang="ts" setup>
/**
 * @file GroupManager.vue
 * @brief 管理群聊和私聊的启用状态、记录、记忆与定时任务
 */
import {computed, inject, nextTick, onMounted, onUnmounted, reactive, ref, type Ref, watch} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {identifierError, SESSION_ID_MAX} from '../../utils/validation'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import NavIcon from '../../components/NavIcon.vue'
import ModalDialog from '../../components/ModalDialog.vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'
import ChatMessageView from './ChatMessageView.vue'
import {useAdminWebSocket} from '../../composables/useAdminWebSocket'
import {useCoalescedRefresh} from '../../composables/useCoalescedRefresh'
import {isPrivateSession, sessionKey, sessionLabel} from '../../utils/session'
import type {
  ApiResponse,
  ChatMessage,
  Group,
  LongTermMemoryEntry,
  LongTermMemoryListResult,
  QQConfig,
  ScheduledTask
} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const qqConfig = inject<QQConfig>('qqConfig')
const wsConnected = inject<Ref<boolean>>('wsConnected') as Ref<boolean>

// 会话列表
const groups: Ref<(Group & { enabled?: boolean })[]> = ref([])
// 保存请求期间收到的绝对计数，防止稍后返回的列表快照覆盖实时更新。
const messageCounts = new Map<string, number>()
const showAddSession = ref(false)
const newSessionType = ref<'group' | 'private'>('group')
const newSessionId = ref('')
const addingSession = ref(false)
const newSessionIdLabel = computed(() => newSessionType.value === 'private' ? 'QQ 号' : '群号')
const sessionForm = useFormValidation(() => ({
  'new-session-id': identifierError(newSessionId.value, newSessionIdLabel.value, false, SESSION_ID_MAX)
}))
const openAddSession = (): void => {
  newSessionType.value = typeFilter.value === 'private' ? 'private' : 'group'
  sessionForm.resetValidation()
  showAddSession.value = true
}
const closeAddSession = (): void => {
  if (addingSession.value || !confirmDiscard(newSessionId.value.trim() !== '')) return
  showAddSession.value = false
  newSessionId.value = ''
}
const addSession = async (): Promise<void> => {
  if (addingSession.value) return
  const id = newSessionId.value.trim()
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
        `${g.groupName ?? ''} ${g.userId ?? ''} ${sessionKey(g)}`.toLowerCase().includes(keyword))
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
    const data = await requestJson<Group[]>('/admin/api/groups', {signal})
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
const isSettingSaving = (group: Group, setting: GroupSetting): boolean =>
    savingSettings.has(`${sessionKey(group)}:${setting}`)
const setGroupSetting = async (group: Group, enabled: boolean, setting: GroupSetting): Promise<void> => {
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
const toggleGroup = async (group: Group, enabled: boolean): Promise<void> => {
  const groupId = sessionKey(group)
  if (savingGroups.has(groupId)) return
  savingGroups.add(groupId)
  try {
    const resp = await fetch(`/admin/api/group/${groupId}/toggle`, {method: 'POST'})
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
    const resp = await fetch(`/admin/api/group/${groupId}`, {method: 'DELETE'})
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
    const records = await requestJson<ChatMessage[]>(`/admin/api/chat-records/${sessionId}?limit=200`)
    if (!Array.isArray(records)) throw new Error('聊天记录数据无效')
    if (disposed || selectedGroup.value !== sessionId) return
    chatRecords.value = records.map((record, index) =>
        record.id === undefined ? {...record, clientKey: `snapshot-${index}`} : record)

    chatError.value = ''
    // 订阅 WebSocket（字符串形式传递，后端可正确解析大数）
    send({action: 'subscribe', groupId: sessionId})
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
  if (selectedGroup.value) send({action: 'unsubscribe', groupId: selectedGroup.value})
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
    const resp = await fetch(`/admin/api/chat-records/${sessionId}/clear`, {method: 'DELETE'})
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
    const data = await requestJson<{memory: string}>(`/admin/api/memory/${sessionId}`)
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
    const data = await requestJson<{tasks: ScheduledTask[]}>(`/admin/api/scheduled-tasks/${sessionId}`)
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

const promptGroup = ref<Group | null>(null)
const promptDraft = ref('')
const savedPrompt = ref('')
const promptLoading = ref(false)
const promptSaving = ref(false)
const promptError = ref('')
const promptBytes = computed(() => new TextEncoder().encode(promptDraft.value).length)
const promptForm = useFormValidation(() => ({'group-prompt': promptBytes.value > 16000 ? '附加提示词不能超过 16000 字节' : ''}))
let promptRequestVersion = 0

const openPrompt = async (group: Group): Promise<void> => {
  promptForm.resetValidation()
  const version = ++promptRequestVersion
  promptGroup.value = group
  promptDraft.value = ''
  savedPrompt.value = ''
  promptError.value = ''
  promptLoading.value = true
  try {
    const response = await fetch(`/admin/api/group/${sessionKey(group)}/prompt`)
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
    (showAddSession.value && newSessionId.value.trim() !== ''),
    () => memorySaving.value || promptSaving.value || addingSession.value || editSaving.value)

const messageKey = (message: DisplayChatMessage): string | number => message.id ?? message.clientKey ?? message.content

const {send} = useAdminWebSocket(event => {
  const data = JSON.parse(event.data)
  if (data.type === 'session_stats_updated') {
    const {groupId, messageCount} = data.data ?? {}
    if (typeof groupId !== 'string' || !Number.isSafeInteger(messageCount) || messageCount < 0) return
    const count = Math.max(messageCounts.get(groupId) || 0, messageCount)
    messageCounts.set(groupId, count)
    const group = groups.value.find(item => sessionKey(item) === groupId)
    if (group) group.messageCount = Math.max(group.messageCount || 0, count)
  }
  if (data.type === 'new_message' && data.groupId === selectedGroup.value) {
    chatRecords.value.push({...data.data, clientKey: `realtime-${++nextRealtimeMessageKey}`})
    nextTick(scrollToBottom)
  }
}, {
  onConnected(socket) {
    void loadGroups()
    if (selectedGroup.value) socket.send(JSON.stringify({action: 'subscribe', groupId: selectedGroup.value}))
  },
  onDisconnected(socket) {
    if (selectedGroup.value && socket.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify({action: 'unsubscribe', groupId: selectedGroup.value}))
  }
})

onMounted(() => {
  if (!wsConnected.value) void loadGroups()
})

onUnmounted(() => {
  disposed = true
  ++promptRequestVersion
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">会话管理</h1>
      <p class="page-subtitle">管理 Bot 启用的群聊与私聊会话、消息与记忆</p>
    </div>

    <template v-if="!selectedGroup">
      <!-- 会话列表 -->
      <div class="card">
        <div class="card-header session-list-header">
          <div style="display: flex; gap: 12px; align-items: center;">
            <h3 class="card-title">会话列表</h3>
            <div :class="{ connected: wsConnected, disconnected: !wsConnected }" class="connection-status">
              <span class="dot"></span>
              {{ wsConnected ? '实时连接' : '未连接' }}
            </div>
          </div>
          <div style="display: flex; flex-wrap: wrap; gap: 8px; align-items: center;">
            <label for="session-search" class="form-label">搜索会话</label>
            <input id="session-search" v-model="searchKeyword" aria-label="搜索会话" class="form-input"
                   placeholder="搜索名称、群号或 QQ 号" type="search" style="width: 240px; max-width: 100%;">
            <div class="filter-tabs">
              <button :class="{ active: typeFilter === 'all' }" class="filter-tab"
                      @click="typeFilter = 'all'">全部
              </button>
              <button :class="{ active: typeFilter === 'group' }" class="filter-tab"
                      @click="typeFilter = 'group'">群聊
              </button>
              <button :class="{ active: typeFilter === 'private' }" class="filter-tab"
                      @click="typeFilter = 'private'">私聊
              </button>
            </div>
            <span style="color: var(--text-secondary); font-size: 13px;">共 {{ filteredGroups.length }} 个会话</span>
            <button :disabled="refreshingNames" class="btn btn-secondary btn-sm" @click="refreshAllGroupNames">{{ refreshingNames ? '刷新中…' : '刷新会话名称' }}</button>
            <button class="btn btn-primary btn-sm" @click="openAddSession">添加会话</button>
          </div>
        </div>
        <div class="table-container">
          <RequestError :message="groupsError" :busy="loading" :stale="groupsLoaded" @retry="loadGroups"/>
          <p v-if="loading && groupsLoaded" role="status">刷新中…</p>
          <template v-if="loading && !groupsLoaded">
            <div class="empty-state">
              <p>加载中...</p>
            </div>
          </template>
          <template v-else-if="!groupsError && filteredGroups.length === 0">
            <div class="empty-state">
              <div class="empty-icon" aria-hidden="true"><NavIcon name="admins"/></div>
              <p>{{ groups.length ? '没有符合筛选条件的会话' : '暂无会话，可点击“添加会话”输入群号或 QQ 号' }}</p>
            </div>
          </template>
          <template v-else-if="filteredGroups.length > 0">
            <table class="session-table">
              <thead>
              <tr>
                <th style="width: 132px;">启用会话</th>
                <th>会话</th>
                <th style="width: 120px;">群号/QQ号</th>
                <th style="width: 70px;">消息</th>
                <th style="width: 90px;">发送群名片</th>
                <th style="width: 90px;">自动回复</th>
                <th style="width: 90px;">命令解析</th>
                <th style="width: 90px;">跨会话共享</th>
                <th class="session-actions-heading">操作</th>
              </tr>
              </thead>
              <tbody>
              <tr v-for="group in filteredGroups" :key="sessionKey(group)" :class="{ 'row-disabled': !group.enabled }"
                  class="group-row"
                  @click="selectGroup(sessionKey(group), sessionLabel(group))">
                <td class="session-enabled-cell" @click.stop>
                  <EnabledSwitch :model-value="group.enabled === true" :disabled="savingGroups.has(sessionKey(group))"
                                 label="启用会话" :title="group.enabled ? '禁用会话' : '启用会话'"
                                 @update:model-value="toggleGroup(group, $event)" />
                </td>
                <td>
                  <strong v-if="isPrivateSession(group)">{{ group.groupName || '私聊' }}</strong>
                  <strong v-else-if="group.groupName">{{ group.groupName }}</strong>
                  <span v-else style="color: var(--text-light)">群 {{ group.groupId }}</span>
                </td>
                <td><code :title="isPrivateSession(group) ? `QQ ${group.userId}` : ''">{{
                    isPrivateSession(group) ? group.userId : group.groupId
                  }}</code></td>
                <td>{{ group.messageCount || 0 }}</td>
                <td @click.stop>
                  <EnabledSwitch v-if="!isPrivateSession(group)" :model-value="group.sendGroupCard === true"
                                 :disabled="isSettingSaving(group, 'sendGroupCard')" :show-status="false"
                                 label="向模型发送本群群名片" :title="group.sendGroupCard ? '关闭发送群名片' : '开启发送群名片'"
                                 @update:model-value="setGroupSetting(group, $event, 'sendGroupCard')" />
                  <span v-else>-</span>
                </td>
                <td @click.stop>
                  <EnabledSwitch v-if="!isPrivateSession(group)" :model-value="group.autoReply !== false"
                                 :disabled="isSettingSaving(group, 'autoReply')" :show-status="false"
                                 label="本群普通消息自动回复" :title="group.autoReply !== false ? '关闭普通消息自动回复' : '开启普通消息自动回复'"
                                 @update:model-value="setGroupSetting(group, $event, 'autoReply')" />
                  <span v-else>-</span>
                </td>
                <td @click.stop>
                  <EnabledSwitch v-if="!isPrivateSession(group)" :model-value="group.commandEnabled !== false"
                                 :disabled="isSettingSaving(group, 'commandEnabled')" :show-status="false"
                                 label="本群命令解析" :title="group.commandEnabled !== false ? '关闭命令解析，命令将按普通消息处理' : '开启命令解析'"
                                 @update:model-value="setGroupSetting(group, $event, 'commandEnabled')" />
                  <span v-else>-</span>
                </td>
                <td @click.stop>
                  <EnabledSwitch :model-value="group.crossSessionSharing === true"
                                 :disabled="isSettingSaving(group, 'crossSessionSharing')" :show-status="false"
                                 label="跨会话共享"
                                 title="与其他开启共享的群聊和私聊互相分享三天内的近期消息"
                                 @update:model-value="setGroupSetting(group, $event, 'crossSessionSharing')" />
                </td>
                <td class="session-actions">
                  <button class="btn btn-secondary btn-sm"
                          @click.stop="viewMemory(sessionKey(group), sessionLabel(group))">会话外号
                  </button>
                  <button class="btn btn-secondary btn-sm" style="margin-left: 6px;"
                          @click.stop="viewSummaries(sessionKey(group), sessionLabel(group))">聊天摘要
                  </button>
                  <button class="btn btn-secondary btn-sm" style="margin-left: 6px;"
                          @click.stop="viewLongTermMemory(sessionKey(group), sessionLabel(group))">长期记忆
                  </button>
                  <button class="btn btn-secondary btn-sm" style="margin-left: 6px;"
                          @click.stop="viewTasks(sessionKey(group), sessionLabel(group))">任务
                  </button>
                  <button v-if="!isPrivateSession(group)" class="btn btn-secondary btn-sm" style="margin-left: 6px;"
                          :title="group.hasAdditionalPrompt ? '已设置群附加提示词' : '未设置群附加提示词'"
                          @click.stop="openPrompt(group)">附加提示词</button>
                  <button class="btn btn-danger btn-sm" style="margin-left: 6px;"
                          :disabled="removingGroups.has(sessionKey(group))" @click.stop="removeGroup(sessionKey(group))">{{ removingGroups.has(sessionKey(group)) ? '删除中…' : '删除' }}
                  </button>
                </td>
              </tr>
              </tbody>
            </table>
          </template>
        </div>
      </div>
    </template>

    <!-- 聊天记录 -->
    <div v-else class="card chat-card">
      <div class="card-header chat-list-header">
        <div class="chat-heading">
          <h3 class="card-title">{{ selectedGroupName }}</h3>
          <span class="msg-count">{{ chatRecords.length }} 条消息</span>
        </div>
        <div class="chat-header-actions">
          <button :disabled="chatRecords.length === 0 || clearingSessions.has(selectedGroup!)" class="btn btn-danger btn-sm" @click="clearGroupRecords">
            {{ clearingSessions.has(selectedGroup!) ? '清空中…' : '清空消息' }}
          </button>
          <button class="btn btn-secondary btn-sm" @click="backToList">返回列表</button>
        </div>
      </div>

      <div ref="chatContainer" class="chat-container">
        <RequestError :message="chatError" :busy="chatLoading" :stale="chatRecords.length > 0" @retry="selectGroup(selectedGroup!, selectedGroupName)"/>
        <div v-if="chatLoading && !chatRecords.length" class="chat-empty">
          <p>加载中...</p>
        </div>
        <template v-else>
          <div
              v-for="msg in chatRecords"
              :key="messageKey(msg)"
              :class="msg.role"
              class="chat-message"
          >
            <!-- 普通显示 -->
            <ChatMessageView v-if="editingId !== msg.id" :message="msg" :bot-name="qqConfig!.botName" :deleting="deletingRecords.has(msg.id!)"
                             @edit="startEdit(msg)" @delete="deleteRecord(msg.id!)" />

            <!-- 编辑模式 -->
            <template v-else>
              <div class="edit-form">
                <label :for="`message-edit-${msg.id}`" class="form-label">消息内容</label>
                <textarea :id="`message-edit-${msg.id}`" v-model="editContent" :disabled="editSaving" class="edit-textarea" rows="3"></textarea>
                <div class="edit-actions">
                  <button :disabled="editSaving" class="btn btn-primary btn-sm" @click="saveEdit">{{ editSaving ? '保存中…' : '保存' }}</button>
                  <button :disabled="editSaving" class="btn btn-secondary btn-sm" @click="cancelEdit">取消</button>
                </div>
              </div>
            </template>
          </div>

          <div v-if="!chatError && chatRecords.length === 0" class="chat-empty">
            <div class="chat-empty-icon" aria-hidden="true"><NavIcon name="groups"/></div>
            <p>暂无聊天消息</p>
          </div>
        </template>
      </div>
    </div>

    <ModalDialog v-if="showAddSession" class="modal-overlay" labelledby="add-session-title" @close="closeAddSession">
      <form class="modal-content" style="max-width: 480px;" novalidate @submit.prevent="addSession">
        <div class="modal-header">
          <h2 id="add-session-title">添加会话</h2>
          <button type="button" :disabled="addingSession" class="btn btn-secondary btn-sm" @click="closeAddSession">关闭</button>
        </div>
        <div class="modal-body">
          <fieldset class="session-type-choice form-group" :disabled="addingSession">
            <legend class="form-label">会话类型</legend>
            <div class="session-type-options">
              <label v-for="type in (['group', 'private'] as const)" :key="type" :class="{selected: newSessionType === type}">
                <input class="sr-only" v-model="newSessionType" type="radio" name="new-session-type" :value="type">
                <span class="choice-mark" aria-hidden="true">{{ newSessionType === type ? '✓' : '' }}</span>
                {{ type === 'group' ? '群聊' : '私聊' }}
              </label>
            </div>
          </fieldset>
          <label for="new-session-id" class="form-label">{{ newSessionIdLabel }}</label>
          <input v-bind="sessionForm.fieldAttrs('new-session-id', 'new-session-id-hint')" v-model="newSessionId" :disabled="addingSession" class="form-input"
                 inputmode="numeric" autocomplete="off"
                 :placeholder="newSessionType === 'private' ? '输入对方的 QQ 号' : '输入机器人已加入的群号'" required>
          <FieldError field="new-session-id" :message="sessionForm.errors.value['new-session-id']"/>
          <p id="new-session-id-hint" class="form-hint">{{ newSessionType === 'private'
              ? '无需已有聊天记录，添加后立即启用该私聊；对方后续发来的消息将按私聊处理。'
              : '添加后将启用该群聊，随后可在列表中调整自动回复、命令及共享设置。' }}</p>
        </div>
        <div class="modal-footer">
          <button type="button" :disabled="addingSession" class="btn btn-secondary" @click="closeAddSession">取消</button>
          <button type="submit" :disabled="addingSession" class="btn btn-primary">{{ addingSession ? '添加中…' : '添加并启用' }}</button>
        </div>
      </form>
    </ModalDialog>

    <!-- 定时任务弹窗 -->
    <ModalDialog v-if="tasksGroupId" class="modal-overlay" labelledby="tasks-title" @close="closeTasks">
      <div class="modal-content">
        <div class="modal-header">
          <h2 id="tasks-title" tabindex="-1" data-modal-initial-focus>{{ tasksGroupName }} - 定时任务</h2>
          <button class="btn btn-secondary btn-sm" @click="closeTasks">关闭</button>
        </div>
        <div class="modal-body">
          <RequestError :message="tasksError" :busy="tasksLoading" :stale="tasksList.length > 0" @retry="viewTasks(tasksGroupId!, tasksGroupName)"/>
          <div v-if="tasksLoading && !tasksList.length" class="memory-loading">
            <p>加载中...</p>
          </div>
          <table v-else-if="tasksList.length > 0" class="affinity-table">
            <thead>
            <tr>
              <th style="width: 60px;">编号</th>
              <th style="width: 160px;">触发时间</th>
              <th>内容</th>
              <th style="width: 80px;"></th>
            </tr>
            </thead>
            <tbody>
            <tr v-for="task in tasksList" :key="task.id">
              <td>#{{ task.id }}</td>
              <td>
                {{ fmtTaskTime(task.remindTime) }}
                <span v-if="task.daily" class="task-daily">每日</span>
                <div class="task-relative">{{ fmtTaskRelative(task.remindTime) }}</div>
              </td>
              <td class="task-content">{{ task.content }}</td>
              <td style="text-align: right;">
                <button :disabled="cancellingTaskId === task.id" class="btn btn-danger btn-sm"
                        @click="cancelTask(task)">{{ cancellingTaskId === task.id ? '取消中' : '取消' }}
                </button>
              </td>
            </tr>
            </tbody>
          </table>
          <div v-else-if="!tasksError" class="memory-loading">
            <p>暂无待触发的定时任务，可在群聊中让 AI 设置提醒</p>
          </div>
        </div>
      </div>
    </ModalDialog>

    <ModalDialog v-if="promptGroup" class="modal-overlay" labelledby="group-prompt-title" @close="closePrompt">
      <div class="modal-content">
        <div class="modal-header">
          <h2 id="group-prompt-title">{{ sessionLabel(promptGroup) }} - 附加提示词</h2>
          <button :disabled="promptSaving" class="btn btn-secondary btn-sm" @click="closePrompt">关闭</button>
        </div>
        <div class="modal-body">
          <div v-if="promptLoading" class="memory-loading" role="status">加载中...</div>
          <div v-else-if="promptError" role="alert">
            <p>{{ promptError }}</p>
            <button class="btn btn-secondary" @click="openPrompt(promptGroup)">重新加载</button>
          </div>
          <template v-else>
            <label for="group-prompt" class="form-label">本群附加提示词</label>
            <textarea v-bind="promptForm.fieldAttrs('group-prompt', 'group-prompt-hint')" v-model="promptDraft" :disabled="promptSaving" class="memory-editor" aria-label="本群附加提示词"
                      placeholder="附加提示词"></textarea>
            <FieldError field="group-prompt" :message="promptForm.errors.value['group-prompt']"/>
            <p id="group-prompt-hint" class="form-hint" :style="promptBytes > 16000 ? {color: 'var(--danger)'} : {}">{{ promptBytes }} / 16000 字节</p>
          </template>
        </div>
        <div class="modal-footer">
          <button :disabled="promptLoading || promptSaving || !!promptError" class="btn btn-secondary" @click="promptDraft = ''">清空内容</button>
          <button :disabled="promptLoading || promptSaving || !!promptError" class="btn btn-primary" @click="savePrompt">{{ promptSaving ? '保存中...' : '保存' }}</button>
        </div>
      </div>
    </ModalDialog>

    <!-- 压缩聊天摘要：按覆盖时间从早到晚展示，不混入原始消息。 -->
    <ModalDialog v-if="summarySession" class="modal-overlay" labelledby="conversation-summary-title" @close="closeSummaries">
      <div class="modal-content">
        <div class="modal-header">
          <h2 id="conversation-summary-title" tabindex="-1" data-modal-initial-focus>{{ summarySession.name }} - 聊天摘要</h2>
          <button class="btn btn-secondary btn-sm" @click="closeSummaries">关闭</button>
        </div>
        <div class="modal-body">
          <p v-if="summariesLoading" class="memory-loading">加载中...</p>
          <div v-else-if="summariesError" class="memory-loading">
            <p role="alert">{{ summariesError }}</p>
            <button class="btn btn-secondary btn-sm" @click="viewSummaries(summarySession.id, summarySession.name)">重试</button>
          </div>
          <p v-else-if="summaries.length === 0" class="memory-loading">暂无聊天摘要，消息触发总结后会显示在这里。</p>
          <ol v-else class="summary-list">
            <li v-for="(item, index) in summaries" :key="index" class="summary-item">
              <div class="summary-time">{{ item.start_time }} — {{ item.end_time }}</div>
              <p class="summary-text">{{ item.summary }}</p>
            </li>
          </ol>
        </div>
      </div>
    </ModalDialog>

    <!-- 会话外号弹窗 -->
    <ModalDialog v-if="memoryGroupId" class="modal-overlay" labelledby="aliases-title" @close="closeMemory">
      <div class="modal-content">
        <div class="modal-header">
          <h2 id="aliases-title">{{ memoryGroupName }} - 会话外号</h2>
          <button class="btn btn-secondary btn-sm" @click="closeMemory">关闭</button>
        </div>
        <div class="modal-body">
          <RequestError :message="memoryError" :busy="memoryLoading" @retry="viewMemory(memoryGroupId!, memoryGroupName)"/>
          <div v-if="memoryLoading" class="memory-loading">
            <p>加载中...</p>
          </div>
          <label v-if="!memoryError && !memoryLoading" for="session-aliases" class="form-label">会话外号（JSON）</label>
          <textarea
              v-if="!memoryError && !memoryLoading" id="session-aliases"
              v-model="groupMemory"
              class="memory-editor"
              placeholder='{"aliases": [{"qq": "123456", "names": ["外号"]}]}'
          ></textarea>
        </div>
        <div class="modal-footer">
          <button :disabled="memorySaving || memoryLoading || !!memoryError" class="btn btn-primary" @click="saveMemory">
            {{ memorySaving ? '保存中...' : '保存' }}
          </button>
        </div>
      </div>
    </ModalDialog>

    <!-- 长期记忆弹窗 -->
    <ModalDialog v-if="ltmGroupId" class="modal-overlay" labelledby="ltm-title" @close="closeLongTermMemory">
      <div class="modal-content">
        <div class="modal-header">
          <h2 id="ltm-title" tabindex="-1" data-modal-initial-focus>{{ ltmGroupName }} - 长期记忆</h2>
          <button class="btn btn-secondary btn-sm" @click="closeLongTermMemory">关闭</button>
        </div>
        <div class="modal-body">
          <RequestError :message="ltmError" :busy="ltmLoading" :stale="ltmList.length > 0" @retry="loadLongTermMemories()"/>
          <div v-if="ltmLoading && !ltmList.length" class="memory-loading">
            <p>加载中...</p>
          </div>
          <template v-else>
            <div v-if="!ltmError && ltmList.length === 0" class="memory-loading">
              <p>暂无长期记忆，等待记忆迁移入库</p>
            </div>
            <div v-else class="ltm-list">
              <div v-for="m in ltmList" :key="m.id" class="ltm-item">
                <div class="task-content">{{ m.content }}</div>
                <div class="ltm-item-footer">
                  <span class="ltm-time">{{ m.createdAt }}</span>
                  <button :disabled="ltmDeletingId === m.id" class="btn btn-danger btn-sm"
                          @click="deleteLongTermMemory(m.id)">
                    {{ ltmDeletingId === m.id ? '删除中...' : '删除' }}
                  </button>
                </div>
              </div>
            </div>
            <div class="ltm-pagination">
              <button :disabled="ltmPage === 0 || ltmLoading" class="btn btn-secondary btn-sm"
                      @click="changeLtmPage(-1)">上一页
              </button>
              <span class="ltm-page-info">第 {{ ltmPage + 1 }} / {{ ltmTotalPages }} 页 · 共 {{ ltmTotal }} 条</span>
              <button :disabled="ltmPage + 1 >= ltmTotalPages || ltmLoading" class="btn btn-secondary btn-sm"
                      @click="changeLtmPage(1)">下一页
              </button>
            </div>
          </template>
        </div>
      </div>
    </ModalDialog>
  </div>
</template>

<style scoped>
.session-type-choice { border: 0; padding: 0; }
.session-type-options { display: flex; border: 1px solid var(--control-border); }
.session-type-options label {
  position: relative; flex: 1; min-height: 42px; padding: 8px 12px; display: flex; align-items: center;
  justify-content: center; gap: 8px; cursor: pointer; color: var(--text-secondary);
}
.session-type-options label + label { border-left: 1px solid var(--control-border); }
.session-type-options label.selected { background: var(--primary-soft); color: var(--primary); font-weight: 600; }
.session-type-options label:focus-within { outline: 2px solid var(--focus-ring); outline-offset: -2px; }

.summary-list {
  margin: 0;
  padding: 0;
  list-style: none;
}

.summary-item + .summary-item {
  border-top: 1px solid var(--border);
  margin-top: 20px;
  padding-top: 20px;
}

.summary-time {
  color: var(--text-light);
  font-size: 13px;
}

.summary-text {
  margin: 10px 0 0;
  line-height: 1.8;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.chat-list-header {
  flex-wrap: wrap;
  gap: 24px 40px;
}

.chat-heading {
  min-width: 0;
}

.chat-header-actions {
  display: flex;
  align-items: center;
  gap: 40px;
  margin-left: auto;
  flex-wrap: wrap;
}

.chat-heading .card-title {
  overflow-wrap: anywhere;
}

.session-list-header {
  flex-wrap: wrap;
  gap: 16px;
}

.session-table {
  min-width: 1060px;
}

.session-table th,
.session-table td {
  padding: 12px;
}

.session-actions-heading {
  width: 1%;
}

.session-actions {
  white-space: nowrap;
}

.session-actions .btn {
  padding-inline: 9px;
}

.session-enabled-cell {
  white-space: nowrap;
}

.row-disabled {
  color: var(--text-secondary);
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--text-secondary);
}

.connection-status .dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
}

.connected .dot {
  background: var(--success);
}

.disconnected .dot {
  background: var(--danger);
}

.group-row {
  cursor: pointer;
}

.filter-tabs {
  display: flex;
  gap: 4px;
  background: transparent;
  border-radius: 0;
  padding: 0;
  border-bottom: 1px solid var(--border);
}

.filter-tab {
  border: none;
  background: none;
  padding: 4px 12px;
  border-radius: 0;
  font-size: 13px;
  cursor: pointer;
  color: var(--text-secondary);
  transition: all 0.2s;
}

.filter-tab:hover {
  color: var(--text-primary);
}

.filter-tab.active {
  background: var(--primary-soft);
  color: var(--primary);
  font-weight: 600;
}

@media (max-width: 640px) {
  .chat-header-actions { width: 100%; margin-left: 0; justify-content: space-between; gap: 16px; }
  .chat-container .chat-message { max-width: 100%; }
}

.group-row:hover td {
  background: var(--primary-softer);
}

.chat-card {
  display: flex;
  flex-direction: column;
  height: max(360px, calc(100dvh - var(--app-header-height) - 56px));
  min-height: 0;
  margin-bottom: 0;
}

.chat-card .card-header {
  flex-shrink: 0;
}

.msg-count {
  font-size: 12px;
  color: var(--text-secondary);
}

.chat-container {
  flex: 1;
  overflow-y: auto;
  padding: 16px 0;
  background: transparent;
  border-top: 1px solid var(--border);
  min-height: 0;
  max-height: none;
}

.chat-message {
  padding: 12px 16px;
  margin-bottom: 12px;
  border-radius: 0;
  max-width: 85%;
  min-width: 0;
  position: relative;
}

.chat-message.user {
  background: var(--card-bg);
  border: 1px solid var(--border);
  margin-right: auto;
}

.chat-message.assistant {
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-left: 3px solid var(--primary);
  color: var(--text-primary);
  margin-left: auto;
}

.edit-form {
  width: 100%;
}

.edit-textarea {
  width: 100%;
  padding: 8px 12px;
  border: 1px solid var(--border);
  border-radius: 0;
  font-size: 14px;
  font-family: inherit;
  resize: vertical;
  background: var(--input-bg);
  color: var(--text-primary);
}

.edit-actions {
  display: flex;
  gap: 8px;
  margin-top: 8px;
}

/* 弹窗 */
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1001;
}

.modal-content {
  background: var(--card-bg);
  border-radius: 0;
  width: 90%;
  max-width: 760px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px 20px;
  border-bottom: 1px solid var(--border);
}

.modal-header h2 {
  margin: 0;
  font-size: 16px;
}

.modal-body {
  flex: 1;
  padding: 16px 20px;
  overflow-y: auto;
  overscroll-behavior: contain;
}

.memory-loading {
  text-align: center;
  padding: 48px;
  color: var(--text-light);
}

.memory-editor {
  width: 100%;
  height: min(400px, 55vh);
  padding: 12px;
  border: 1px solid var(--control-border);
  border-radius: 0;
  font-size: 13px;
  font-family: inherit;
  resize: vertical;
  line-height: 1.6;
  background: var(--input-bg);
  color: var(--text-primary);
}

.memory-editor:focus {
  outline: 2px solid var(--focus-ring);
  outline-offset: 2px;
  border-color: var(--primary);
}

.modal-footer {
  padding: 12px 20px;
  border-top: 1px solid var(--border);
  display: flex;
  justify-content: flex-end;
}

/* 好感度 */
.affinity-table {
  width: 100%;
  border-collapse: collapse;
}

.affinity-table th,
.affinity-table td {
  padding: 8px 12px;
  border-bottom: 1px solid var(--border);
  text-align: left;
  font-size: 14px;
}

.affinity-table tbody tr:last-child td {
  border-bottom: none;
}

/* 定时任务 */
.task-daily {
  display: inline-block;
  margin-left: 6px;
  padding: 0 6px;
  border-radius: 0;
  font-size: 11px;
  background: var(--bg-secondary);
  color: var(--primary);
}

.task-relative {
  font-size: 12px;
  color: var(--primary);
}

.task-content {
  word-break: break-word;
}

/* 长期记忆 */
.ltm-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.ltm-item {
  border: 1px solid var(--border-color);
  border-radius: 0;
  padding: 10px 12px;
}

.ltm-item-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-top: 6px;
}

.ltm-time {
  font-size: 12px;
  color: var(--text-light);
}

.ltm-pagination {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  margin-top: 12px;
}

.ltm-page-info {
  font-size: 13px;
  color: var(--text-secondary);
}
</style>
