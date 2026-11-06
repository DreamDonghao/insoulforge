<script setup lang="ts">
import {ref,type UnwrapNestedRefs} from 'vue'
import type {useLogs} from '../shared/features/diagnostics/useLogs'
const {state:s}=defineProps<{state:UnwrapNestedRefs<ReturnType<typeof useLogs>>}>()
const filters=ref(false)
</script>
<template>
  <div class="m-logs">
    <div class="m-log-actions"><button @click="filters=!filters">{{ s.currentGroupLabel }} · {{ s.level==='all'?'全部级别':s.level }} ⌄</button><button @click="s.toggleFollow()">{{ s.followLatest?'暂停':'跟踪' }}</button><button :disabled="s.loading" @click="s.refresh()">刷新</button></div>
    <form v-if="filters" class="m-filters" @submit.prevent="s.refresh();filters=false"><label>日志范围<select v-model="s.groupId"><option value="all">全部日志</option><option value="system">系统日志</option><option v-for="g in s.groups" :key="s.sessionKey(g)" :value="s.sessionKey(g)">{{ s.sessionLabel(g) }}</option></select></label><label>日志级别<select v-model="s.level"><option v-for="level in ['all','trace','debug','info','warn','error']" :key="level" :value="level">{{ level==='all'?'全部':level }}</option></select></label><label>关键字<input v-model="s.keyword" placeholder="搜索日志"></label><button type="button" @click="s.resetFilters()">重置</button><button>应用筛选</button></form>
    <small role="status">{{ s.connectionLabel }} · 当前级别 {{ s.currentLevel }} · 缓存 {{ s.size }} 条</small>
    <p v-if="s.groupsError" role="alert">{{ s.groupsError }}<button @click="s.loadGroups()">重试范围</button></p>
    <p v-if="s.loadError" role="alert">{{ s.loadError }}<button :disabled="s.loading" @click="s.loadLogs(s.retryAppend)">重试</button></p>
    <button v-if="s.hasMore" class="older" :disabled="s.loading" @click="s.loadOlder()">{{ s.loading?'加载中…':'加载更早' }}</button>
    <div :ref="el=>s.logListEl=el as HTMLElement" class="m-log-list" @scroll="s.onScroll">
      <p v-if="s.loading&&!s.loaded" role="status">加载中…</p><p v-if="s.loaded&&!s.entries.length">暂无日志</p>
      <article v-for="entry in s.entries" :key="entry.id"><div class="m-log-meta"><time>{{ entry.timestamp }}</time><strong :class="'level-'+entry.level">{{ entry.level }}</strong></div><p>{{ entry.content }}</p><details><summary>来源详情</summary><small>{{ s.groupTag(entry.sessionId) }} · {{ entry.source }}</small></details></article>
    </div>
  </div>
</template>
<style scoped>
.m-logs{font-size:14px;min-width:0}.m-log-actions{display:flex;gap:6px;align-items:center;margin-bottom:8px}.m-log-actions button:first-child{flex:1;text-align:left;min-width:0;overflow-wrap:anywhere}button{font:inherit;min-height:44px;padding:8px 10px;border:0;background:var(--card-bg);color:var(--text-primary);border-radius:8px}small{font-size:12px;color:var(--text-secondary)}.m-filters{padding:12px;background:var(--card-bg);border-radius:10px;margin-bottom:8px;display:grid;grid-template-columns:1fr 1fr;gap:12px}.m-filters label:nth-child(3){grid-column:1/-1}label{font-size:12px}input,select{display:block;width:100%;min-height:44px;font-size:16px;padding:8px;margin-top:4px;border:1px solid var(--control-border);border-radius:8px;background:var(--input-bg);color:var(--text-primary)}.m-log-list{height:calc(100dvh - 188px);min-height:280px;overflow:auto;overscroll-behavior:contain;margin-top:8px}article{padding:10px 0;border-bottom:1px solid var(--border)}.m-log-meta{display:flex;justify-content:space-between;gap:8px;font-size:12px;color:var(--text-secondary)}strong{font-weight:500}p{font:13px/1.65 monospace;white-space:pre-wrap;overflow-wrap:anywhere;margin:6px 0}summary{min-height:44px;display:flex;align-items:center;font-size:12px;color:var(--text-secondary);padding:6px 0;cursor:pointer}.level-warn{color:var(--warning)}.level-error{color:var(--danger)}.older{width:100%;margin-top:8px}
</style>
