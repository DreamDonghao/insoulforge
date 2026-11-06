<script setup lang="ts">
import {reactive} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'

import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import NavIcon from '../../components/NavIcon.vue'

import EnabledSwitch from '../../components/EnabledSwitch.vue'
import ChatMessageView from './ChatMessageView.vue'


import {isPrivateSession, sessionKey, sessionLabel} from '../../utils/session'
import type {ApiResponse, ChatMessage, SessionSummary, LongTermMemoryEntry, LongTermMemoryListResult, QQConfig, ScheduledTask} from '../../vite-env'
import {useSessions} from '../../shared/features/conversation/useSessions'
import SessionDialogs from '../../shared/components/SessionDialogs.vue'
const state = useSessions()
const dialogState=reactive(state)
const {showToast, qqConfig, wsConnected, groups, messageCounts, showAddSession, newSessionType, newSessionId, addingSession, newSessionIdLabel, sessionForm, openAddSession, closeAddSession, addSession, searchKeyword, typeFilter, filteredGroups, selectedGroup, selectedGroupName, chatRecords, chatContainer, chatLoading, chatError, nextRealtimeMessageKey, disposed, editingId, editContent, editSaving, editRevision, savedEditContent, messageDirty, memoryGroupId, memoryGroupName, groupMemory, savedMemory, memoryError, memoryLoading, memorySaving, summarySession, summaries, summariesLoading, summariesError, closeSummaries, viewSummaries, groupsError, groupsLoaded, loadGroups, loading, groupSettings, savingSettings, isSettingSaving, setGroupSetting, removingGroups, deletingRecords, clearingSessions, refreshingNames, savingGroups, toggleGroup, removeGroup, refreshAllGroupNames, selectGroup, backToList, scrollToBottom, startEdit, finishEdit, cancelEdit, saveEdit, deleteRecord, clearGroupRecords, viewMemory, closeMemory, saveMemory, tasksGroupId, tasksGroupName, tasksList, tasksLoading, tasksError, cancellingTaskId, fmtTaskTime, fmtTaskRelative, viewTasks, closeTasks, cancelTask, LTM_PAGE_SIZE, ltmGroupId, ltmGroupName, ltmList, ltmTotal, ltmPage, ltmLoading, ltmError, ltmRequestVersion, ltmDeletingId, ltmTotalPages, loadLongTermMemories, viewLongTermMemory, closeLongTermMemory, changeLtmPage, deleteLongTermMemory, promptGroup, promptDraft, savedPrompt, promptLoading, promptSaving, promptError, promptBytes, promptForm, promptRequestVersion, openPrompt, closePrompt, savePrompt, messageKey, send} = state
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
                   placeholder="搜索名称、群号或用户 ID" type="search" style="width: 240px; max-width: 100%;">
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
        <div class="table-container session-list-table">
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
              <p>{{ groups.length ? '没有符合筛选条件的会话' : '暂无会话，可点击“添加会话”输入群号或用户 ID' }}</p>
            </div>
          </template>
          <template v-else-if="filteredGroups.length > 0">
            <table class="session-table">
              <thead>
              <tr>
                <th class="session-enabled-heading">启用会话</th>
                <th class="session-name-heading">会话</th>
                <th class="session-id-heading">群号/用户 ID</th>
                <th class="session-count-heading">消息</th>
                <th class="session-setting-heading">发送群名片</th>
                <th class="session-setting-heading">自动回复</th>
                <th class="session-setting-heading">命令解析</th>
                <th class="session-setting-heading">跨会话共享</th>
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
                  <strong v-if="isPrivateSession(group)">{{ group.sessionName || '私聊' }}</strong>
                  <strong v-else-if="group.sessionName">{{ group.sessionName }}</strong>
                  <span v-else style="color: var(--text-light)">群 {{ group.targetId }}</span>
                </td>
                <td><code :title="isPrivateSession(group) ? `用户 ID ${group.targetId}` : ''">{{
                    isPrivateSession(group) ? group.targetId : group.targetId
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
                  <div class="session-action-buttons">
                    <button class="btn btn-secondary btn-sm"
                            @click.stop="viewMemory(sessionKey(group), sessionLabel(group))">会话外号
                    </button>
                    <button class="btn btn-secondary btn-sm"
                            @click.stop="viewSummaries(sessionKey(group), sessionLabel(group))">聊天摘要
                    </button>
                    <button class="btn btn-secondary btn-sm"
                            @click.stop="viewLongTermMemory(sessionKey(group), sessionLabel(group))">长期记忆
                    </button>
                    <button class="btn btn-secondary btn-sm"
                            @click.stop="viewTasks(sessionKey(group), sessionLabel(group))">任务
                    </button>
                    <button v-if="!isPrivateSession(group)" class="btn btn-secondary btn-sm"
                            :title="group.hasAdditionalPrompt ? '已设置群附加提示词' : '未设置群附加提示词'"
                            @click.stop="openPrompt(group)">附加提示词</button>
                    <button class="btn btn-danger btn-sm"
                            :disabled="removingGroups.has(sessionKey(group))" @click.stop="removeGroup(sessionKey(group))">{{ removingGroups.has(sessionKey(group)) ? '删除中…' : '删除' }}
                    </button>
                  </div>
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

    <SessionDialogs :state="dialogState"/>
  </div>
</template>

<style scoped>
.session-type-choice { border: 0; padding: 0; }
.session-type-options { display: flex; border: 1px solid var(--control-border); border-radius: var(--radius-sm); }
.session-type-options label {
  position: relative; flex: 1; min-height: 42px; padding: 8px 12px; display: flex; align-items: center;
  justify-content: center; gap: 8px; cursor: pointer; color: var(--text-secondary);
}
.session-type-options label + label { border-left: 1px solid var(--control-border); }
.session-type-options label.selected { background: var(--primary-soft); color: var(--primary); font-weight: 600; }
.session-type-options label:focus-within { outline: 2px solid var(--focus-ring); outline-offset: -2px; }
.session-type-options label:first-child { border-radius: var(--radius-sm) 0 0 var(--radius-sm); }
.session-type-options label:last-child { border-radius: 0 var(--radius-sm) var(--radius-sm) 0; }

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

.session-enabled-heading { width: 132px; }
.session-id-heading { width: 120px; }
.session-count-heading { width: 70px; }
.session-setting-heading { width: 90px; }
.session-actions-heading { width: 1%; }
.session-action-buttons { display: flex; flex-wrap: wrap; gap: 6px; }
.session-list-table { container: session-list / inline-size; }

.session-actions {
  white-space: nowrap;
}

.session-actions .btn {
  padding-inline: 9px;
}

.session-enabled-cell {
  white-space: nowrap;
}

@media (min-width: 1100px) {
  .session-table { min-width: 0; }
  .session-table th, .session-table td { padding: 12px 6px; }
  .session-table .session-enabled-heading { width: 12%; }
  .session-table .session-name-heading { width: 14%; }
  .session-table .session-id-heading { width: 14%; }
  .session-table .session-count-heading { width: 6%; }
  .session-table .session-setting-heading { width: 8%; }
  .session-table .session-actions-heading { width: 22%; }
  .session-table .session-actions, .session-table .session-enabled-cell { white-space: normal; }
  .session-enabled-cell :deep(.tool-switch) { flex-wrap: wrap; gap: 4px; white-space: normal; }
  .session-action-buttons .btn { max-width: 100%; white-space: normal; overflow-wrap: anywhere; }
}

/* 内容区足够宽时，按实际内容分配列宽，优先完整显示一行操作。 */
@container session-list (min-width: 960px) {
  .session-table { table-layout: auto; }
  .session-table th[class] { width: auto; }
  .session-table th.session-actions-heading { width: 1%; }
  .session-action-buttons { flex-wrap: nowrap; }
  .session-action-buttons .btn { flex-shrink: 0; white-space: nowrap; }
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
  background: var(--bg-secondary);
  border-radius: var(--radius-sm);
  padding: 0;
  border-bottom: 0;
}

.filter-tab {
  border: none;
  background: none;
  padding: 4px 12px;
  border-radius: var(--radius-sm);
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
  padding: 16px;
  background: var(--bg-secondary);
  border-top: 0;
  min-height: 0;
  max-height: none;
  border-radius: var(--radius-md);
}

.chat-message {
  padding: 12px 16px;
  margin-bottom: 12px;
  border-radius: var(--radius-md);
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
  border-radius: var(--radius-sm);
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
  border-radius: var(--radius-lg);
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
  border-radius: var(--radius-sm);
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
  border-radius: var(--radius-xs);
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
  border-radius: var(--radius-sm);
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
