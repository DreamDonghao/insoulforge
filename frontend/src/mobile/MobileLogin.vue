<script setup lang="ts">
import {ref} from 'vue'
import NavIcon from '../components/NavIcon.vue'
defineProps<{checked:boolean,token:string,error:string,busy:boolean,themeLabel:string}>()
const emit=defineEmits<{'update:token':[value:string],submit:[],theme:[]}>()
const visible=ref(false)
</script>
<template>
  <main class="mobile-login"><header><strong>InSoulForge</strong><button type="button" @click="emit('theme')">{{ themeLabel }}</button></header><section><h1>登录管理后台</h1><p>使用访问令牌登录</p><p v-if="!checked" role="status">正在验证登录状态…</p><form v-else @submit.prevent="emit('submit')"><label for="mobile-login-token">访问令牌</label><div class="secret-input"><input id="mobile-login-token" class="form-input" :type="visible?'text':'password'" :value="token" :disabled="busy" autocomplete="current-password" required @input="emit('update:token',($event.target as HTMLInputElement).value)"><button class="secret-toggle" type="button" :aria-label="visible?'隐藏访问令牌':'显示访问令牌'" :aria-pressed="visible" @click="visible=!visible"><NavIcon :name="visible?'eye-off':'eye'"/></button></div><p v-if="error" class="error" role="alert">{{ error }}</p><button class="submit" :disabled="busy">{{ busy?'验证中…':'进入后台' }}</button></form></section></main>
</template>
<style scoped>
.mobile-login{min-height:100dvh;padding:16px;font-size:14px;background:var(--bg-main);color:var(--text-primary)}header{display:flex;align-items:center;justify-content:space-between;gap:12px}header strong{font-size:18px}button{min-height:44px;padding:8px 12px;font:inherit;color:inherit;border:0;border-radius:8px;background:var(--card-bg)}section{max-width:400px;margin:15dvh auto 0}h1{font-size:22px;margin:0 0 8px}p{font-size:14px;color:var(--text-secondary);margin:0 0 24px}label{display:block;margin-bottom:8px}.form-input{font-size:16px;min-height:44px}.submit{width:100%;margin-top:16px;background:var(--primary-strong);color:var(--on-primary)}.error{margin:12px 0 0;color:var(--danger)}button:disabled{opacity:.6}
</style>
