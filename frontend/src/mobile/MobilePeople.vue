<script setup lang="ts">
import {inject, nextTick, onMounted, onBeforeUnmount, ref, type UnwrapNestedRefs} from 'vue'
import type {usePeople} from '../shared/features/conversation/usePeople'
const {state:s}=defineProps<{state: UnwrapNestedRefs<ReturnType<typeof usePeople>>}>()
const detailOpen=ref(false)
const more=ref(false)
let listTop=0
const registerBack = inject<(handler:()=>void)=>void>('mobileBack',()=>{})
registerBack(back)
const setDetail = inject<(active:boolean)=>void>('mobileDetail',()=>{})
async function open(id:string){listTop=window.scrollY;detailOpen.value=true;setDetail(true);history.pushState({},'', '#mobile/people/'+encodeURIComponent(id));await s.selectPerson(id);window.scrollTo(0,0)}
function back(){if(s.editing){s.cancelEdit();return}detailOpen.value=false;setDetail(false);history.replaceState({},'','#mobile/people');void nextTick(()=>window.scrollTo(0,listTop))}
function restore(){if(!location.hash.startsWith('#mobile/people'))return;const id=location.hash.split('/')[2];detailOpen.value=!!id;setDetail(!!id);if(id&&id!==s.selected)void s.selectPerson(decodeURIComponent(id))}
onMounted(()=>{restore();window.addEventListener('popstate',restore)})
onBeforeUnmount(()=>{setDetail(false);window.removeEventListener('popstate',restore)})
</script>
<template>
  <div class="m-people">
    <template v-if="!detailOpen">
      <input v-model="s.search" class="m-input" aria-label="搜索人物" placeholder="搜索昵称或用户 ID">
      <div class="m-inline"><select v-model="s.sortBy" aria-label="排序依据"><option v-if="s.affinityEnabled" value="affinity">按好感度</option><option value="tokens">按 Token</option></select><button @click="s.ascending=!s.ascending">{{ s.ascending?'升序':'降序' }}</button><button :disabled="s.loading" @click="s.loadPeople()">刷新</button><small>{{ s.filtered.length }} 人</small></div>
      <p v-if="s.error" role="alert">{{ s.error }}</p><p v-if="s.loading&&!s.people.length" role="status">加载中…</p>
      <div class="m-group"><button v-for="p in s.filtered" :key="p.user_id" class="m-person" @click="open(p.user_id)"><span><strong>{{ p.name||'未知' }}</strong><small>{{ p.user_id }} · {{ p.estimated_tokens||0 }} Token · {{ p.pending_count }} 条待整理</small></span><span v-if="s.affinityEnabled" :class="s.scoreClass(p.affinity)">{{ (p.affinity||0)>0?'+':'' }}{{ p.affinity }}</span><span>›</span></button></div><p v-if="!s.loading&&!s.filtered.length">暂无匹配人物</p>
    </template>
    <template v-else>
      <div class="m-inline"><button @click="back">‹ {{ s.editing?'取消编辑':'人物列表' }}</button><button v-if="!s.editing" @click="more=!more">更多操作</button></div>
      <p v-if="s.detailLoading" role="status">加载中…</p><p v-if="s.detailError" role="alert">{{ s.detailError }}<button @click="s.selectPerson(s.selected)">重试</button></p>
      <template v-if="s.detail">
        <h2>{{ s.detailName }}</h2><small>用户 ID {{ s.detail.user_id }}</small>
        <div v-if="more&&!s.editing" class="m-inline"><button :disabled="s.consolidating" @click="s.consolidatePerson()">立即整理</button><button :disabled="s.consolidating" @click="s.consolidatePerson(true)">直接压缩</button><button @click="s.beginEdit();more=false">编辑</button><button class="danger" @click="s.deletePerson()">删除人物</button></div>
        <p v-if="s.consolidationMessage" role="status">{{ s.consolidationMessage }}</p><p v-if="s.editError" role="alert">{{ s.editError }}</p>
        <form v-if="s.editing" @submit.prevent="s.savePerson()"><label for="mobile-person-json">人物数据 JSON</label><textarea id="mobile-person-json" :ref="el=>s.jsonEditor=el as HTMLTextAreaElement" v-model="s.draftText" :disabled="s.saving" :aria-invalid="s.jsonInvalid" spellcheck="false"/><button :disabled="s.saving">{{ s.saving?'保存中…':'保存修改' }}</button></form>
        <template v-else><div class="m-inline"><span>{{ s.detail.estimated_tokens||0 }} Token</span><span v-if="s.affinityEnabled">好感度 {{ s.detail.affinity }}</span></div><section v-for="section in s.memorySections" :key="section.key" class="m-group"><h3>{{ section.title }}</h3><p v-if="!section.items.length">暂无记录</p><p v-for="(item,i) in section.items" :key="i">{{ item }}</p></section><section class="m-group"><h3>待整理记忆</h3><p v-if="!s.detail.pending_entries.length">暂无记录</p><article v-for="entry in s.detail.pending_entries" :key="entry.id"><p>{{ entry.content }}</p><small>{{ s.formatTime(entry.time) }} · {{ s.sourceLabel(entry.source_session) }}</small></article></section></template>
      </template>
    </template>
  </div>
</template>
<style scoped>
.m-people{font-size:14px}.m-input,select,textarea{width:100%;min-height:44px;font-size:16px;background:var(--input-bg);color:var(--text-primary);border:1px solid var(--control-border);border-radius:8px;padding:8px}
.m-inline{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin:8px 0 12px}.m-inline select{width:auto;flex:1;min-width:0}button{font:inherit;min-height:44px;border:0;border-radius:8px;background:var(--card-bg);color:var(--text-primary);padding:8px 12px}button:disabled{opacity:.5}
.m-group{background:var(--card-bg);border-radius:10px;margin:12px 0;padding:0 12px}.m-person{display:flex;align-items:center;gap:12px;justify-content:space-between;width:100%;text-align:left;padding:12px 0;border-radius:0;border-bottom:1px solid var(--border)}.m-person>span:first-child{flex:1;min-width:0;overflow-wrap:anywhere}strong{font-weight:500}small{display:block;font-size:12px;color:var(--text-secondary);margin-top:4px}.m-person small{line-height:1.5}.positive{color:var(--success)}.negative,.danger{color:var(--danger)}h2{font-size:18px;margin:12px 0 4px}h3{font-size:14px;margin:0;padding-top:12px}p{white-space:pre-wrap;overflow-wrap:anywhere;padding-bottom:12px}textarea{display:block;min-height:55dvh;margin:8px 0 12px;font-family:monospace;resize:vertical}
</style>
