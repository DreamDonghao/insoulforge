<script setup lang="ts">

import {usePeople} from './usePeople'
import {ArrowDownWideNarrow, ArrowUpNarrowWide, Pencil, Trash2} from '@lucide/vue'

const state = usePeople()
const {people, selected, detail, search, loading, detailLoading, error, detailError, ascending, sortBy, sortLabel, affinityEnabled, editing, draftText, saving, savedDraft, isDirty, consolidating, compressing, consolidationMessage, editError, jsonInvalid, jsonEditor, focusJsonEditor, nameTimer, disposed, summaryController, detailController, sortedPeople, filtered, detailName, memorySections, scoreClass, sourceLabel, formatTime, fetchPeopleJson, selectPerson, loadPeople, beginEdit, cancelEdit, savePerson, deletePerson, consolidatePerson} = state
</script>

<template>
  <div class="people-page">
    <div class="page-header"><h1 class="page-title">人物信息</h1><p class="page-subtitle">{{ affinityEnabled ? '人物记忆与全局好感度' : '人物记忆' }}</p></div>
    <div class="people-toolbar">
      <label for="people-search" class="form-label">搜索人物</label>
      <input id="people-search" v-model="search" class="form-input" placeholder="搜索昵称或用户 ID" aria-label="搜索人物">
      <span>{{ filtered.length }} / {{ people.length }} 人</span>
      <div class="sort-options" role="group" aria-label="人物排序依据">
        <button v-if="affinityEnabled" class="btn btn-secondary" :aria-pressed="sortBy === 'affinity'" @click="sortBy = 'affinity'">好感度</button>
        <button class="btn btn-secondary" :aria-pressed="sortBy === 'tokens' || !affinityEnabled" @click="sortBy = 'tokens'">预估 Token</button>
      </div>
      <button class="btn btn-secondary icon-button" :title="sortLabel + (ascending ? '：从低到高' : '：从高到低')" :aria-label="sortLabel + (ascending ? '：从低到高' : '：从高到低')" @click="ascending = !ascending"><ArrowUpNarrowWide v-if="ascending" :size="18"/><ArrowDownWideNarrow v-else :size="18"/></button>
      <button class="btn btn-secondary" :disabled="loading || editing || saving || consolidating" @click="loadPeople()">刷新</button>
    </div>
    <p v-if="error" role="alert">{{ error }}</p>
    <p v-else-if="loading && !people.length" class="empty">加载中...</p>
    <p v-else-if="!people.length" class="empty">暂无人物记录</p>
    <div v-else class="people-layout">
      <aside class="people-list" aria-label="人物列表">
        <p v-if="!filtered.length" class="empty">没有匹配的人物</p>
        <button v-for="person in filtered" :key="person.user_id" class="person-row"
                :class="{selected: selected === person.user_id}" :aria-pressed="selected === person.user_id"
                :disabled="consolidating || saving"
                @click="selectPerson(person.user_id)">
          <span class="person-identity"><strong>{{ person.name || '未知' }}</strong><small>{{ person.user_id }} · {{ person.pending_count }} 条待整理 · {{ person.estimated_tokens ?? 0 }} 预估 Token</small></span>
          <span v-if="affinityEnabled" :class="scoreClass(person.affinity)">{{ (person.affinity ?? 0) > 0 ? '+' : '' }}{{ person.affinity }}</span>
        </button>
      </aside>
      <section class="person-detail" aria-live="polite" :aria-busy="detailLoading">
        <p v-if="detailLoading" class="empty">加载中...</p>
        <p v-else-if="detailError" role="alert">{{ detailError }}</p>
        <template v-else-if="detail">
          <div class="person-actions" v-if="!editing">
            <button class="btn btn-secondary" :disabled="saving || consolidating" @click="consolidatePerson()">{{ consolidating && !compressing ? '整理中...' : '立即整理' }}</button>
            <button class="btn btn-secondary" :disabled="saving || consolidating" @click="consolidatePerson(true)">{{ consolidating && compressing ? '压缩中...' : '直接压缩' }}</button>
            <button class="btn btn-secondary" :disabled="saving || consolidating" @click="beginEdit"><Pencil :size="16"/>编辑</button>
            <button class="btn btn-danger icon-button" title="删除人物" aria-label="删除人物" :disabled="saving || consolidating" @click="deletePerson"><Trash2 :size="16"/></button>
          </div>
          <p v-if="consolidationMessage" role="status">{{ consolidationMessage }}</p>
          <p v-if="editError" id="person-json-error" role="alert">{{ editError }}</p>
          <form v-if="editing" class="person-editor" @submit.prevent="savePerson">
            <fieldset :disabled="saving">
              <header class="person-heading editor-heading">
                <div><h2>{{ detailName }}</h2><span>用户 ID {{ selected }}</span></div>
              </header>
              <label for="person-json" class="form-label">人物数据 JSON</label>
              <textarea id="person-json" ref="jsonEditor" :aria-invalid="jsonInvalid || undefined" :aria-describedby="editError ? 'person-json-error' : undefined" v-model="draftText" class="form-input person-json" aria-label="人物数据 JSON" spellcheck="false"/>
              <footer class="person-actions editor-footer"><button type="button" class="btn btn-secondary" @click="cancelEdit">取消</button><button class="btn btn-primary" type="submit">{{ saving ? '保存中...' : '保存' }}</button></footer>
            </fieldset>
          </form>
          <template v-else>
          <header class="person-heading">
            <div><h2>{{ detailName }}</h2><span>用户 ID {{ detail.user_id }}</span></div>
            <div class="person-stats"><div class="score" title="按实际人物上下文估算；0 可能表示尚未计算"><span>预估 Token</span><strong>{{ (detail.estimated_tokens ?? 0).toLocaleString() }}</strong></div>
            <div v-if="affinityEnabled" class="score"><span>全局好感度</span><strong :class="scoreClass(detail.affinity)">{{ (detail.affinity ?? 0) > 0 ? '+' : '' }}{{ detail.affinity }}</strong></div></div>
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
.sort-options, .person-stats { display: flex; gap: 12px; }
.sort-options .btn[aria-pressed="true"] { border-color: var(--primary); background: var(--bg-secondary); box-shadow: inset 0 -2px var(--primary); }
.people-toolbar { display: flex; align-items: center; gap: 16px; margin-bottom: 24px; background: var(--card-bg); padding: 16px; border-radius: var(--radius-md); }
.people-toolbar input { max-width: 420px; min-width: 0; flex: 1; }
.people-toolbar span, .muted, small, .person-heading span { color: var(--text-secondary); }
.people-layout { display: grid; grid-template-columns: minmax(220px, 300px) minmax(0, 1fr); flex: 1; min-height: 0; overflow: hidden; border-top: 0; gap: 16px; }
.people-list, .person-detail { min-height: 0; overflow-y: auto; overscroll-behavior-y: contain; scrollbar-gutter: stable; }
.people-list { border-right: 0; border-bottom: 0; background: var(--card-bg); border-radius: var(--radius-md); }
.person-row { display: flex; justify-content: space-between; align-items: center; gap: 16px; width: 100%; min-height: 76px; padding: 16px; border: 0; border-bottom: 1px solid var(--border-color); background: transparent; color: var(--text-primary); text-align: left; cursor: pointer; border-radius: var(--radius-sm); }
.person-row:hover { background: var(--primary-softer); }
.person-row.selected { background: var(--primary-soft); }
.person-row:focus-visible { outline: 2px solid var(--focus-ring); outline-offset: -3px; }
.person-identity strong { font-weight: 500; }
.person-row.selected .person-identity strong { color: var(--primary); font-weight: 700; }
.person-identity { display: grid; gap: 6px; min-width: 0; overflow-wrap: anywhere; }
.person-detail { min-width: 0; padding: 24px; background: var(--card-bg); border-radius: var(--radius-md); }
.person-heading { flex-wrap: wrap; display: flex; justify-content: space-between; gap: 24px; align-items: center; padding-bottom: 24px; overflow-wrap: anywhere; }
h2, h3, p { margin-top: 0; }
h2 { font-size: 22px; margin-bottom: 8px; }
h3 { font-size: 16px; margin-bottom: 16px; }
h3 small { margin-left: 8px; font-weight: normal; }
.score { display: grid; gap: 8px; text-align: right; flex-shrink: 0; }
.score strong { font-size: 26px; }
.positive { color: var(--success); } .negative { color: var(--danger); }
.memory-section { border-top: 0; padding: 16px; background: var(--bg-secondary); margin-bottom: 16px; border-radius: var(--radius-sm); }
.memory-entry { padding: 12px 0; border-bottom: 1px solid var(--border-color); }
.memory-entry:last-child { border-bottom: 0; }
.memory-entry p, li { white-space: pre-wrap; overflow-wrap: anywhere; line-height: 1.7; margin-bottom: 8px; }
.empty { padding: 32px 16px; color: var(--text-secondary); text-align: center; }
@media (max-width: 760px) {
  .people-page { height: auto; min-height: 0; }
  .people-layout { display: flex; flex-direction: column; flex: initial; overflow: visible; }
  .people-list { flex-shrink: 0; max-height: 304px; scrollbar-gutter: auto; }
  .person-detail { padding: 16px; overflow: visible; scrollbar-gutter: auto; }
  .person-actions { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .person-actions .btn { width: 100%; min-width: 0; justify-content: center; }
  .person-actions .icon-button { height: 44px; }
  .person-heading { gap: 16px; padding-bottom: 16px; }
  .editor-heading { flex-wrap: wrap; }
  .people-toolbar { flex-wrap: wrap; gap: 12px; margin-bottom: 16px; }
  .people-toolbar input { flex-basis: 100%; max-width: none; order: 1; }
  .people-toolbar .sort-options, .people-toolbar > button { order: 2; }
  .people-toolbar > .form-label { margin: 0; }
  .people-toolbar > span { margin-left: auto; }
}
</style>
