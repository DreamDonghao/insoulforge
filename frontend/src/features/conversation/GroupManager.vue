<script lang="ts" setup>
/**
 * @file GroupManager.vue
 * @brief 管理群聊和私聊的启用状态、记录、记忆与定时任务
 */
import {computed, inject, nextTick, onMounted, onUnmounted, reactive, ref, type Ref, watch} from 'vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'
import ChatMessageView from './ChatMessageView.vue'
import {useAdminWebSocket} from '../../composables/useAdminWebSocket'
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
const loading: Ref<boolean> = ref(false)
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
let nextRealtimeMessageKey = 0
let disposed = false

// 编辑状态
const editingId: Ref<number | null> = ref(null)
const editContent: Ref<string> = ref('')

// 记忆弹窗
const memoryGroupId: Ref<string | null> = ref(null)
const memoryGroupName: Ref<string> = ref('')
const groupMemory: Ref<string> = ref('')
const memoryLoading: Ref<boolean> = ref(false)
const memorySaving: Ref<boolean> = ref(false)

// 加载群列表
const loadGroups = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch('/admin/api/groups')
    if (!resp.ok) {
      showToast!('加载失败: ' + resp.status, true)
      groups.value = []
      return
    }
    const data = await resp.json()
    if (Array.isArray(data)) {
      groups.value = data
    } else {
      groups.value = []
    }
  } catch {
    showToast!('网络错误，请检查后端服务', true)
    groups.value = []
  } finally {
    loading.value = false
  }
}

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
  if (!confirm('确定要删除该会话吗？聊天记录将保留。')) return
  const resp = await fetch(`/admin/api/group/${groupId}`, {method: 'DELETE'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('已删除')
    await loadGroups()
  }
}

// 刷新已登记会话的名称
const refreshAllGroupNames = async (): Promise<void> => {
  const resp = await fetch('/admin/api/groups/refresh-names', {method: 'POST'})
  const data = await resp.json()
  if (data.success) {
    await loadGroups()
    showToast!('群名称已刷新')
  }
}

// 选择会话查看聊天记录
const selectGroup = async (sessionId: string, sessionName: string): Promise<void> => {
  selectedGroup.value = sessionId
  selectedGroupName.value = sessionName
  chatLoading.value = true
  chatRecords.value = []

  try {
    const resp = await fetch(`/admin/api/chat-records/${sessionId}?limit=200`)
    const records: ChatMessage[] = await resp.json()
    if (disposed || selectedGroup.value !== sessionId) return
    chatRecords.value = records.map((record, index) =>
        record.id === undefined ? {...record, clientKey: `snapshot-${index}`} : record)

    // 订阅 WebSocket（字符串形式传递，后端可正确解析大数）
    send({action: 'subscribe', groupId: sessionId})
  } finally {
    if (!disposed && selectedGroup.value === sessionId) chatLoading.value = false
  }

  // 等待消息列表渲染完成后滚动到底部
  await nextTick()
  scrollToBottom()
}

// 返回列表
const backToList = (): void => {
  send({action: 'unsubscribe'})
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
  if (record.id === undefined) return
  editingId.value = record.id
  editContent.value = record.content
}

// 取消编辑
const cancelEdit = (): void => {
  editingId.value = null
  editContent.value = ''
}

// 保存编辑
const saveEdit = async (): Promise<void> => {
  if (!editingId.value || !editContent.value.trim()) return

  const resp = await fetch(`/admin/api/chat-record/${editingId.value}`, {
    method: 'PUT',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({content: editContent.value})
  })
  const data: ApiResponse = await resp.json()
  if (data.success) {
    const record = chatRecords.value.find(r => r.id === editingId.value)
    if (record) record.content = editContent.value
    showToast!('已更新')
    cancelEdit()
  }
}

// 删除记录
const deleteRecord = async (recordId: number): Promise<void> => {
  if (!confirm('确定删除这条记录？')) return

  const resp = await fetch(`/admin/api/chat-record/${recordId}`, {method: 'DELETE'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    chatRecords.value = chatRecords.value.filter(r => r.id !== recordId)
    showToast!('已删除')
  }
}

// 清空当前会话的聊天记录
const clearGroupRecords = async (): Promise<void> => {
  if (!selectedGroup.value) return
  if (!confirm(
    '确定清空该会话的所有聊天记录？此操作不可恢复！\n' +
    '若当前有记忆总结任务正在执行，清空后收到的新消息也可能在该任务完成时被删除。这不会撤回群内消息。',
  )) return

  try {
    const resp = await fetch(`/admin/api/chat-records/${selectedGroup.value}/clear`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) {
      showToast!(data.error || '清空失败', true)
      return
    }
    chatRecords.value = []
    showToast!('聊天记录已清空')
    await loadGroups()
  } catch {
    showToast!('清空失败，请检查网络与后端服务', true)
  }
}

// 查看记忆
const viewMemory = async (sessionId: string, sessionName: string): Promise<void> => {
  memoryGroupId.value = sessionId
  memoryGroupName.value = sessionName
  memoryLoading.value = true
  groupMemory.value = ''

  try {
    const resp = await fetch(`/admin/api/memory/${sessionId}`)
    const data = await resp.json()
    groupMemory.value = data.memory || ''
  } finally {
    memoryLoading.value = false
  }
}

// 关闭记忆弹窗
const closeMemory = (): void => {
  memoryGroupId.value = null
  memoryGroupName.value = ''
  groupMemory.value = ''
}

// 保存记忆
const saveMemory = async (): Promise<void> => {
  if (!memoryGroupId.value) return
  memorySaving.value = true
  try {
    const resp = await fetch(`/admin/api/memory/${memoryGroupId.value}`, {
      method: 'PUT',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({memory: groupMemory.value})
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('记忆已保存')
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } finally {
    memorySaving.value = false
  }
}

// 定时任务弹窗
const tasksGroupId: Ref<string | null> = ref(null)
const tasksGroupName: Ref<string> = ref('')
const tasksList: Ref<ScheduledTask[]> = ref([])
const tasksLoading: Ref<boolean> = ref(false)
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
  tasksGroupId.value = sessionId
  tasksGroupName.value = sessionName
  tasksLoading.value = true
  tasksList.value = []

  try {
    const resp = await fetch(`/admin/api/scheduled-tasks/${sessionId}`)
    const data = await resp.json()
    tasksList.value = Array.isArray(data.tasks) ? data.tasks : []
  } finally {
    tasksLoading.value = false
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
    if (data.success) {
      tasksList.value = tasksList.value.filter(t => t.id !== task.id)
      showToast!('定时任务已取消')
    } else {
      showToast!(data.error || '取消失败', true)
    }
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
const ltmDeletingId: Ref<number | null> = ref(null)
const ltmTotalPages = computed(() => Math.max(1, Math.ceil(ltmTotal.value / LTM_PAGE_SIZE)))

const loadLongTermMemories = async (): Promise<void> => {
  if (!ltmGroupId.value) return
  ltmLoading.value = true
  try {
    const params = new URLSearchParams({
      sessionId: ltmGroupId.value,
      limit: String(LTM_PAGE_SIZE),
      offset: String(ltmPage.value * LTM_PAGE_SIZE)
    })
    const resp = await fetch(`/admin/api/long-term-memory?${params}`)
    const data: LongTermMemoryListResult = await resp.json()
    ltmList.value = data.items || []
    ltmTotal.value = data.total || 0
  } finally {
    ltmLoading.value = false
  }
}

// 查看长期记忆
const viewLongTermMemory = async (sessionId: string, sessionName: string): Promise<void> => {
  ltmGroupId.value = sessionId
  ltmGroupName.value = sessionName
  ltmPage.value = 0
  ltmList.value = []
  ltmTotal.value = 0
  await loadLongTermMemories()
}

// 关闭长期记忆弹窗
const closeLongTermMemory = (): void => {
  ltmGroupId.value = null
  ltmGroupName.value = ''
  ltmList.value = []
  ltmTotal.value = 0
}

const changeLtmPage = (delta: number): void => {
  ltmPage.value += delta
  loadLongTermMemories()
}

// 删除长期记忆
const deleteLongTermMemory = async (id: number): Promise<void> => {
  if (!confirm('确定删除这条长期记忆？删除后无法恢复')) return
  ltmDeletingId.value = id
  try {
    const resp = await fetch(`/admin/api/long-term-memory/${id}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('长期记忆已删除')
      await loadLongTermMemories()
    } else {
      showToast!(data.error || '删除失败', true)
    }
  } finally {
    ltmDeletingId.value = null
  }
}

const promptGroup = ref<Group | null>(null)
const promptDraft = ref('')
const promptLoading = ref(false)
const promptSaving = ref(false)
const promptError = ref('')
const promptBytes = computed(() => new TextEncoder().encode(promptDraft.value).length)
let promptRequestVersion = 0

const openPrompt = async (group: Group): Promise<void> => {
  const version = ++promptRequestVersion
  promptGroup.value = group
  promptDraft.value = ''
  promptError.value = ''
  promptLoading.value = true
  try {
    const response = await fetch(`/admin/api/group/${sessionKey(group)}/prompt`)
    if (!response.ok) throw new Error(`附加提示词加载失败（HTTP ${response.status}）`)
    const data: {additionalPrompt: string} = await response.json()
    if (typeof data.additionalPrompt !== 'string') throw new Error('附加提示词数据无效')
    if (version === promptRequestVersion) promptDraft.value = data.additionalPrompt
  } catch (error) {
    if (version === promptRequestVersion) promptError.value = error instanceof Error ? error.message : '加载失败'
  } finally {
    if (version === promptRequestVersion) promptLoading.value = false
  }
}

const closePrompt = (): void => {
  if (promptSaving.value) return
  ++promptRequestVersion
  promptGroup.value = null
}

const savePrompt = async (): Promise<void> => {
  if (!promptGroup.value || promptLoading.value || promptSaving.value || promptError.value || promptBytes.value > 16000) return
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
    showToast?.('群附加提示词已保存')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    promptSaving.value = false
  }
}

// 弹窗打开时锁定页面滚动，滚轮只作用于弹窗内容
const anyModalOpen = computed(() =>
    !!(memoryGroupId.value || ltmGroupId.value || tasksGroupId.value || promptGroup.value))
watch(anyModalOpen, (open) => {
  document.body.style.overflow = open ? 'hidden' : ''
})

const messageKey = (message: DisplayChatMessage): string | number => message.id ?? message.clientKey ?? message.content

const {send} = useAdminWebSocket(event => {
  const data = JSON.parse(event.data)
  if (data.type === 'new_message' && data.groupId === selectedGroup.value) {
    chatRecords.value.push({...data.data, clientKey: `realtime-${++nextRealtimeMessageKey}`})
    nextTick(scrollToBottom)
  }
}, {
  onConnected(socket) {
    if (selectedGroup.value) socket.send(JSON.stringify({action: 'subscribe', groupId: selectedGroup.value}))
  },
  onDisconnected(socket) {
    if (selectedGroup.value && socket.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify({action: 'unsubscribe'}))
  }
})

onMounted(loadGroups)

onUnmounted(() => {
  disposed = true
  ++promptRequestVersion
  if (anyModalOpen.value) document.body.style.overflow = ''
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
            <input v-model="searchKeyword" aria-label="搜索会话" class="form-input"
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
            <button class="btn btn-secondary btn-sm" @click="refreshAllGroupNames">刷新会话名称</button>
          </div>
        </div>
        <div class="table-container">
          <template v-if="loading">
            <div class="empty-state">
              <p>加载中...</p>
            </div>
          </template>
          <template v-else-if="filteredGroups.length === 0">
            <div class="empty-state">
              <div class="empty-icon">👥</div>
              <p>{{ groups.length ? '没有符合筛选条件的会话' : '暂无会话，请通过 /enable 命令启用' }}</p>
            </div>
          </template>
          <template v-else>
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
                          @click.stop="viewMemory(sessionKey(group), sessionLabel(group))">短期记忆
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
                          @click.stop="removeGroup(sessionKey(group))">删除
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
          <button :disabled="chatRecords.length === 0" class="btn btn-danger btn-sm" @click="clearGroupRecords">
            清空消息
          </button>
          <button class="btn btn-secondary btn-sm" @click="backToList">返回列表</button>
        </div>
      </div>

      <div ref="chatContainer" class="chat-container">
        <div v-if="chatLoading" class="chat-empty">
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
            <ChatMessageView v-if="editingId !== msg.id" :message="msg" :bot-name="qqConfig!.botName"
                             @edit="startEdit(msg)" @delete="deleteRecord(msg.id!)" />

            <!-- 编辑模式 -->
            <template v-else>
              <div class="edit-form">
                <textarea v-model="editContent" class="edit-textarea" rows="3"></textarea>
                <div class="edit-actions">
                  <button class="btn btn-success btn-sm" @click="saveEdit">保存</button>
                  <button class="btn btn-secondary btn-sm" @click="cancelEdit">取消</button>
                </div>
              </div>
            </template>
          </div>

          <div v-if="chatRecords.length === 0" class="chat-empty">
            <div class="chat-empty-icon">💬</div>
            <p>暂无聊天消息</p>
          </div>
        </template>
      </div>
    </div>

    <!-- 定时任务弹窗 -->
    <div v-if="tasksGroupId" class="modal-overlay" @click.self="closeTasks">
      <div class="modal-content">
        <div class="modal-header">
          <h2>{{ tasksGroupName }} - 定时任务</h2>
          <button class="btn btn-secondary btn-sm" @click="closeTasks">关闭</button>
        </div>
        <div class="modal-body">
          <div v-if="tasksLoading" class="memory-loading">
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
          <div v-else class="memory-loading">
            <p>暂无待触发的定时任务，可在群聊中让 AI 设置提醒</p>
          </div>
        </div>
      </div>
    </div>

    <div v-if="promptGroup" class="modal-overlay" @click.self="closePrompt" @keydown.esc="closePrompt">
      <div class="modal-content" role="dialog" aria-modal="true" aria-labelledby="group-prompt-title">
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
            <textarea v-model="promptDraft" :disabled="promptSaving" class="memory-editor" aria-label="本群附加提示词"
                      :aria-invalid="promptBytes > 16000" placeholder="附加提示词"></textarea>
            <p class="form-hint" :style="promptBytes > 16000 ? {color: 'var(--danger)'} : {}">{{ promptBytes }} / 16000 字节</p>
          </template>
        </div>
        <div class="modal-footer">
          <button :disabled="promptLoading || promptSaving || !!promptError" class="btn btn-secondary" @click="promptDraft = ''">清空内容</button>
          <button :disabled="promptLoading || promptSaving || !!promptError || promptBytes > 16000" class="btn btn-primary" @click="savePrompt">{{ promptSaving ? '保存中...' : '保存' }}</button>
        </div>
      </div>
    </div>

    <!-- 短期记忆弹窗 -->
    <div v-if="memoryGroupId" class="modal-overlay" @click.self="closeMemory">
      <div class="modal-content">
        <div class="modal-header">
          <h2>{{ memoryGroupName }} - 短期记忆</h2>
          <button class="btn btn-secondary btn-sm" @click="closeMemory">关闭</button>
        </div>
        <div class="modal-body">
          <div v-if="memoryLoading" class="memory-loading">
            <p>加载中...</p>
          </div>
          <textarea
              v-else
              v-model="groupMemory"
              class="memory-editor"
              placeholder="暂无短期记忆，可在此编辑..."
          ></textarea>
        </div>
        <div class="modal-footer">
          <button :disabled="memorySaving" class="btn btn-primary" @click="saveMemory">
            {{ memorySaving ? '保存中...' : '保存' }}
          </button>
        </div>
      </div>
    </div>

    <!-- 长期记忆弹窗 -->
    <div v-if="ltmGroupId" class="modal-overlay" @click.self="closeLongTermMemory">
      <div class="modal-content">
        <div class="modal-header">
          <h2>{{ ltmGroupName }} - 长期记忆</h2>
          <button class="btn btn-secondary btn-sm" @click="closeLongTermMemory">关闭</button>
        </div>
        <div class="modal-body">
          <div v-if="ltmLoading" class="memory-loading">
            <p>加载中...</p>
          </div>
          <template v-else>
            <div v-if="ltmList.length === 0" class="memory-loading">
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
    </div>
  </div>
</template>

<style scoped>
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
  opacity: 0.5;
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
  background: var(--bg-secondary);
  border-radius: 0;
  padding: 3px;
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
  background: var(--card-bg);
  color: var(--primary);
  font-weight: 600;
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
  padding: 16px;
  background: var(--bg-secondary);
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
  border: 1px solid var(--border);
  border-radius: 0;
  font-size: 13px;
  font-family: inherit;
  resize: vertical;
  line-height: 1.6;
  background: var(--input-bg);
  color: var(--text-primary);
}

.memory-editor:focus {
  outline: none;
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
