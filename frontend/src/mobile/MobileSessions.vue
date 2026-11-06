<script setup lang="ts">
import {computed,inject,onBeforeUnmount,ref} from 'vue'
import type {SessionSummary} from '../vite-env'
import {sessionKey,sessionLabel,isPrivateSession} from '../utils/session'
import EnabledSwitch from '../components/EnabledSwitch.vue'
type Session = SessionSummary & {enabled?:boolean}
interface State {
 searchKeyword:string;typeFilter:'all'|'group'|'private';filteredGroups:Session[];groupsError:string;loading:boolean;refreshingNames:boolean;
 savingGroups:Set<string>;removingGroups:Set<string>;
 loadGroups:()=>unknown;refreshAllGroupNames:()=>unknown;openAddSession:()=>unknown;
 toggleGroup:(g:Session,value:boolean)=>unknown;setGroupSetting:(g:Session,value:boolean,key:'sendGroupCard'|'autoReply'|'commandEnabled'|'crossSessionSharing')=>unknown;
 isSettingSaving:(g:Session,key:'sendGroupCard'|'autoReply'|'commandEnabled'|'crossSessionSharing')=>boolean;
 selectGroup:(id:string,name:string)=>unknown;viewMemory:(id:string,name:string)=>unknown;viewSummaries:(id:string,name:string)=>unknown;
 viewLongTermMemory:(id:string,name:string)=>unknown;viewTasks:(id:string,name:string)=>unknown;openPrompt:(g:Session)=>unknown;removeGroup:(id:string)=>unknown
}
const {state:s}=defineProps<{state:State}>()
const selected=ref('')
const active=computed(()=>s.filteredGroups.find(g=>sessionKey(g)===selected.value))
const detail=inject<(active:boolean)=>void>('mobileDetail',()=>{})
const registerBack=inject<(handler:()=>void)=>void>('mobileBack',()=>{})
let top=0
function back(){selected.value='';detail(false);requestAnimationFrame(()=>window.scrollTo(0,top))}
async function remove(){if(!active.value)return;await s.removeGroup(sessionKey(active.value));if(!active.value)back()}
function open(g:Session){top=window.scrollY;selected.value=sessionKey(g);detail(true);window.scrollTo(0,0)}
registerBack(back)
onBeforeUnmount(()=>detail(false))
const options=[{key:'sendGroupCard',label:'发送群名片'},{key:'autoReply',label:'普通消息自动回复'},{key:'commandEnabled',label:'命令解析'},{key:'crossSessionSharing',label:'跨会话共享'}] as const
</script>
<template>
 <div class="m-sessions">
  <template v-if="!active">
   <div class="search"><input v-model="s.searchKeyword" type="search" aria-label="搜索会话" placeholder="搜索会话、群号或用户 ID"><button @click="s.openAddSession()">添加</button></div>
   <div class="filters" role="group" aria-label="会话类型"><button v-for="item in [{key:'all',label:'全部'},{key:'group',label:'群聊'},{key:'private',label:'私聊'}]" :key="item.key" :aria-pressed="s.typeFilter===item.key" @click="s.typeFilter=item.key as State['typeFilter']">{{ item.label }}</button><small>{{ s.filteredGroups.length }} 个</small></div>
   <p v-if="s.groupsError" role="alert">{{ s.groupsError }}<button @click="s.loadGroups()">重试</button></p><p v-if="s.loading" role="status">加载中…</p>
   <div class="list"><button v-for="g in s.filteredGroups" :key="sessionKey(g)" class="session-row" @click="open(g)"><span class="avatar">{{ isPrivateSession(g)?'私':'群' }}</span><span class="identity"><strong>{{ sessionLabel(g) }}</strong><small>{{ g.targetId }} · {{ g.messageCount||0 }} 条消息</small></span><span class="status" :class="{enabled:g.enabled}">{{ g.enabled?'启用':'停用' }}</span><span>›</span></button></div>
   <p v-if="!s.loading&&!s.groupsError&&!s.filteredGroups.length">暂无匹配会话</p>
   <details><summary>列表操作</summary><button :disabled="s.refreshingNames" @click="s.refreshAllGroupNames()">{{ s.refreshingNames?'刷新中…':'刷新会话名称' }}</button><button :disabled="s.loading" @click="s.loadGroups()">重新加载</button></details>
  </template>
  <template v-else>
   <h2>{{ sessionLabel(active) }}</h2><small>{{ isPrivateSession(active)?'私聊':'群聊' }} · {{ active.targetId }}</small>
   <section class="settings"><div class="setting"><span>启用会话</span><EnabledSwitch :model-value="active.enabled===true" :disabled="s.savingGroups.has(sessionKey(active))" :show-status="false" label="启用会话" @update:model-value="s.toggleGroup(active,$event)"/></div>
    <template v-for="option in options" :key="option.key"><div v-if="option.key==='crossSessionSharing'||!isPrivateSession(active)" class="setting"><span>{{ option.label }}</span><EnabledSwitch :model-value="option.key==='autoReply'||option.key==='commandEnabled'?active[option.key]!==false:active[option.key]===true" :disabled="s.isSettingSaving(active,option.key)" :show-status="false" :label="option.label" @update:model-value="s.setGroupSetting(active,$event,option.key)"/></div></template>
   </section>
   <div class="actions"><button @click="s.selectGroup(sessionKey(active),sessionLabel(active))">聊天记录 <span>{{ active.messageCount||0 }} 条 ›</span></button><button @click="s.viewMemory(sessionKey(active),sessionLabel(active))">会话外号 <span>›</span></button><button @click="s.viewSummaries(sessionKey(active),sessionLabel(active))">聊天摘要 <span>›</span></button><button @click="s.viewLongTermMemory(sessionKey(active),sessionLabel(active))">长期记忆 <span>›</span></button><button @click="s.viewTasks(sessionKey(active),sessionLabel(active))">任务 <span>›</span></button><button v-if="!isPrivateSession(active)" @click="s.openPrompt(active)">附加提示词 <span>{{ active.hasAdditionalPrompt?'已设置':'未设置' }} ›</span></button></div>
   <button class="remove" :disabled="s.removingGroups.has(sessionKey(active))" @click="remove">删除会话</button>
  </template>
 </div>
</template>
<style scoped>
.m-sessions{font-size:14px}button{font:inherit;min-height:44px;border:0;padding:8px 12px;background:var(--card-bg);color:var(--text-primary);border-radius:8px;cursor:pointer}button:disabled{opacity:.5}button:focus-visible,input:focus-visible{outline:2px solid var(--focus-ring);outline-offset:-2px}.search{display:flex;gap:8px}input{flex:1;min-width:0;height:44px;font-size:16px;border:1px solid var(--control-border);border-radius:8px;padding:8px 10px;background:var(--input-bg);color:var(--text-primary)}.filters{display:flex;align-items:center;gap:4px;margin:8px 0}.filters button{background:none}.filters button[aria-pressed=true]{color:var(--primary);font-weight:600}.filters small{margin-left:auto}.list,.settings,.actions{border-radius:10px;background:var(--card-bg);overflow:hidden;margin:12px 0}.session-row{display:flex;align-items:center;gap:10px;width:100%;border-radius:0;text-align:left;padding:12px;border-bottom:1px solid var(--border)}.avatar{display:grid;place-items:center;width:36px;height:36px;flex-shrink:0;background:var(--bg-secondary);border-radius:10px;color:var(--text-secondary)}.identity{flex:1;min-width:0;overflow-wrap:anywhere}strong{font-weight:500}small{font-size:12px;color:var(--text-secondary)}.identity small{display:block;margin-top:4px}.status{font-size:12px;color:var(--text-secondary)}.status.enabled{color:var(--success)}h2{font-size:18px;margin:12px 0 4px}.setting{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:8px 12px;min-height:52px;border-bottom:1px solid var(--border)}.actions button{width:100%;display:flex;justify-content:space-between;border-bottom:1px solid var(--border);border-radius:0;text-align:left}.actions span{color:var(--text-secondary);font-size:12px}.remove{width:100%;color:var(--danger)}summary{min-height:44px;display:flex;align-items:center;color:var(--text-secondary);font-size:12px}p{overflow-wrap:anywhere}
</style>
