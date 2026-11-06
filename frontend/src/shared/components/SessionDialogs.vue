<script setup lang="ts">
import {toRefs, type UnwrapNestedRefs} from 'vue'
import type {useSessions} from '../features/conversation/useSessions'
import FieldError from '../../components/FieldError.vue'
import RequestError from '../../components/RequestError.vue'

import ModalDialog from '../../components/ModalDialog.vue'


import {sessionLabel} from '../../utils/session'
const props=defineProps<{state:UnwrapNestedRefs<ReturnType<typeof useSessions>>}>()
const {showAddSession, newSessionType, newSessionId, addingSession, newSessionIdLabel, sessionForm, closeAddSession, addSession, memoryGroupId, memoryGroupName, groupMemory, memoryError, memoryLoading, memorySaving, summarySession, summaries, summariesLoading, summariesError, closeSummaries, viewSummaries, viewMemory, closeMemory, saveMemory, tasksGroupId, tasksGroupName, tasksList, tasksLoading, tasksError, cancellingTaskId, fmtTaskTime, fmtTaskRelative, viewTasks, closeTasks, cancelTask, ltmGroupId, ltmGroupName, ltmList, ltmTotal, ltmPage, ltmLoading, ltmError, ltmDeletingId, ltmTotalPages, loadLongTermMemories, closeLongTermMemory, changeLtmPage, deleteLongTermMemory, promptGroup, promptDraft, promptLoading, promptSaving, promptError, promptBytes, promptForm, openPrompt, closePrompt, savePrompt}=toRefs(props.state)
</script>
<template>
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
                  autocomplete="off"
                 :placeholder="newSessionType === 'private' ? '输入对方的用户 ID' : '输入机器人已加入的群号'" required>
          <FieldError field="new-session-id" :message="sessionForm.errors['new-session-id']"/>
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
            <FieldError field="group-prompt" :message="promptForm.errors['group-prompt']"/>
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
              placeholder='{"aliases": [{"user_id": "123456", "names": ["外号"]}]}'
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
