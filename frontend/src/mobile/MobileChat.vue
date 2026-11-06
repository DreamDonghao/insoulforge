<script setup lang="ts">
import {inject,onMounted,onBeforeUnmount} from 'vue'
import type {ChatMessage} from '../vite-env'
import ChatMessageView from '../features/conversation/ChatMessageView.vue'
interface State {selectedGroup:string|null;selectedGroupName:string;chatRecords:ChatMessage[];chatContainer:HTMLDivElement|null;chatError:string;chatLoading:boolean;editingId:number|null;editContent:string;editSaving:boolean;deletingRecords:Set<number>;clearingSessions:Set<string>;backToList:()=>unknown;clearGroupRecords:()=>unknown;selectGroup:(id:string,name:string)=>unknown;startEdit:(msg:ChatMessage)=>unknown;deleteRecord:(id:number)=>unknown;saveEdit:()=>unknown;cancelEdit:()=>unknown}
const {state:s,botName}=defineProps<{state:State,botName:string}>()
const detail=inject<(value:boolean)=>void>('mobileDetail',()=>{})
const register=inject<(handler:()=>void)=>void>('mobileBack',()=>{})
onMounted(()=>{detail(true);register(()=>s.backToList())})
onBeforeUnmount(()=>detail(false))
</script>
<template>
 <div class="mobile-chat">
  <div class="chat-top"><div><h2>{{ s.selectedGroupName }}</h2><small>{{ s.chatRecords.length }} 条消息</small></div><details><summary>更多</summary><button :disabled="!s.chatRecords.length||s.clearingSessions.has(s.selectedGroup!)" @click="s.clearGroupRecords()">清空消息</button></details></div>
  <p v-if="s.chatError" role="alert">{{ s.chatError }}<button @click="s.selectGroup(s.selectedGroup!,s.selectedGroupName)">重试</button></p>
  <p v-if="s.chatLoading&&!s.chatRecords.length" role="status">加载中…</p>
  <div :ref="el=>s.chatContainer=el as HTMLDivElement" class="messages">
   <article v-for="(msg,i) in s.chatRecords" :key="msg.id??i"><ChatMessageView v-if="s.editingId!==msg.id" :message="msg" :bot-name="botName" :deleting="s.deletingRecords.has(msg.id!)" @edit="s.startEdit(msg)" @delete="s.deleteRecord(msg.id!)"/><form v-else @submit.prevent="s.saveEdit()"><label :for="'mobile-message-'+msg.id">消息内容</label><textarea :id="'mobile-message-'+msg.id" v-model="s.editContent" :disabled="s.editSaving"/><div class="edit-buttons"><button :disabled="s.editSaving">{{ s.editSaving?'保存中…':'保存' }}</button><button type="button" :disabled="s.editSaving" @click="s.cancelEdit()">取消</button></div></form></article>
   <p v-if="!s.chatLoading&&!s.chatError&&!s.chatRecords.length">暂无聊天消息</p>
  </div>
 </div>
</template>
<style scoped>
.mobile-chat{font-size:14px;min-width:0}.chat-top{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:12px}h2{font-size:16px;overflow-wrap:anywhere;margin:0 0 4px}small{font-size:12px;color:var(--text-secondary)}summary,button{min-height:44px;padding:10px;cursor:pointer}button{font:inherit;border:0;border-radius:8px;background:var(--card-bg);color:var(--text-primary)}details button{color:var(--danger)}.messages{max-height:calc(100dvh - 150px);overflow:auto;overscroll-behavior:contain}article{padding:12px;margin:8px 0;background:var(--card-bg);border-radius:10px;overflow-wrap:anywhere}textarea{display:block;width:100%;min-height:160px;font-size:16px;background:var(--input-bg);color:var(--text-primary);border:1px solid var(--control-border);border-radius:8px;margin:8px 0;padding:8px}.edit-buttons{display:flex;gap:8px}p{overflow-wrap:anywhere}
</style>
