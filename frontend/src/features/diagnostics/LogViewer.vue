<script setup lang="ts">

import {useLogs} from './useLogs'

import RequestError from '../../components/RequestError.vue'
const state = useLogs()
const {sessionKey, sessionLabel, groups, entries, loading, loadError, loaded, retryAppend, groupsError, groupsLoading, followLatest, connectionState, connectionLabel, hasMore, nextBeforeId, currentLevel, size, groupId, level, keyword, limit, activeFilters, replacingSnapshot, liveDuringSnapshot, matchesFilters, logListEl, logWs, reconnectTimer, scrollFrame, forceScroll, disposed, logRequest, logController, groupsController, scrollToBottom, groupLabels, groupTag, currentGroupLabel, levelClass, subscribe, connectWebSocket, disconnectWebSocket, loadGroups, buildQuery, applySnapshot, loadLogs, refresh, loadOlder, resetFilters, handleWsMessage, onScroll, toggleFollow} = state
</script>

<template>
  <div class="log-viewer">
    <div class="page-header">
      <div class="page-title">运行日志</div>
      <div class="page-subtitle">{{ currentGroupLabel }} · 当前级别 {{ currentLevel }} · {{ size }} 条缓存</div>
    </div>

    <RequestError :message="groupsError" :busy="groupsLoading" :stale="groups.length > 0" @retry="loadGroups"/>
    <div class="card log-toolbar">
      <div class="form-row">
        <div class="form-group">
          <label for="log-scope" class="form-label">日志范围</label>
          <select id="log-scope" v-model="groupId" class="form-input" @change="refresh">
            <option value="all">全部日志</option>
            <option value="system">系统日志</option>
            <option v-for="g in groups" :key="sessionKey(g)" :value="sessionKey(g)">
              {{ sessionLabel(g) }}
            </option>
          </select>
        </div>
        <div class="form-group">
          <label for="log-level" class="form-label">日志级别</label>
          <select id="log-level" v-model="level" class="form-input" @change="refresh">
            <option value="all">全部</option>
            <option value="trace">trace</option>
            <option value="debug">debug</option>
            <option value="info">info</option>
            <option value="warn">warn</option>
            <option value="error">error</option>
          </select>
        </div>
        <div class="form-group">
          <label for="log-keyword" class="form-label">关键字</label>
          <input id="log-keyword" v-model="keyword" class="form-input" placeholder="输入关键字后回车" @keyup.enter="refresh">
        </div>
      </div>
      <div class="log-actions">
        <button class="btn btn-secondary" type="button" @click="resetFilters">重置</button>
        <button class="btn btn-secondary" type="button" @click="refresh">刷新</button>
        <button class="btn btn-primary" type="button" @click="toggleFollow">
          {{ followLatest ? '暂停跟踪' : '继续跟踪' }}
        </button>
      </div>
    </div>

    <div class="card log-list-card">
      <div ref="logListEl" class="log-list" @scroll="onScroll">
        <RequestError :message="loadError" :busy="loading" :stale="loaded" @retry="loadLogs(retryAppend)"/>
        <p v-if="loading && !loaded" role="status">加载中…</p>
        <div v-else-if="!loadError && entries.length === 0" class="log-empty">暂无日志</div>
        <div v-for="entry in entries" :key="entry.id" v-memo="[entry, groupLabels]"
             :class="['log-line', levelClass(entry.level)]">
          <span class="log-time">{{ entry.timestamp }}</span>
          <span class="log-level">{{ entry.level }}</span>
          <span :title="entry.sessionId === null ? '系统' : groupTag(entry.sessionId)" class="log-group">{{
              entry.sessionId === null ? '系统' : groupTag(entry.sessionId)
            }}</span>
          <span class="log-source">{{ entry.source }}</span>
          <span class="log-message">{{ entry.content }}</span>
        </div>
      </div>

      <div class="log-footer">
        <button v-if="hasMore" class="btn btn-secondary" type="button" @click="loadOlder">加载更早</button>
        <span v-else-if="loaded && !loadError" class="form-hint">已显示全部可用日志</span>
        <span role="status" class="form-hint">{{ connectionLabel }}</span>
        <span class="form-hint">自动滚动 {{ followLatest ? '开启' : '关闭' }}</span>
      </div>
    </div>
  </div>
</template>
