<script lang="ts" setup>
/**
 * @file App.vue
 * @brief 主应用组件 - 管理后台入口
 */
import {computed, onMounted, provide, reactive, ref, type Ref} from 'vue'
import {useToast} from './composables/useToast'
import type {QQConfig as QQConfigType} from './vite-env.d'

import NavIcon from './components/NavIcon.vue'
import Dashboard from './components/Dashboard.vue'
import LLMConfig from './components/LLMConfig.vue'
import PromptEditor from './components/PromptEditor.vue'
import EmojiManager from './components/EmojiManager.vue'
import AdminManager from './components/AdminManager.vue'
import GroupManager from './components/GroupManager.vue'
import MemoryConfig from './components/MemoryConfig.vue'
import QQConfigVue from './components/QQConfig.vue'
import CustomTools from './components/CustomTools.vue'
import UsageStats from './components/UsageStats.vue'
import LogViewer from './components/LogViewer.vue'
import RequestDebug from './components/RequestDebug.vue'
import About from './components/About.vue'

interface NavItem {
  key: string
  label: string
  icon: string
}

// 导航配置
const systemNavItems: NavItem[] = [
  {key: 'llm', label: 'LLM配置', icon: 'llm'},
  {key: 'qqConfig', label: 'OneBot 配置', icon: 'qq'},
  {key: 'prompts', label: '提示词', icon: 'prompts'},
  {key: 'customTools', label: '自定义工具', icon: 'tool'},
  {key: 'memoryConfig', label: '记忆与上下文', icon: 'memory'}
]

const dailyNavItems: NavItem[] = [
  {key: 'dashboard', label: '首页', icon: 'home'},
  {key: 'emojis', label: '表情库', icon: 'emojis'},
  {key: 'admins', label: '管理员', icon: 'admins'},
  {key: 'groups', label: '会话管理', icon: 'groups'},
  {key: 'logs', label: '运行日志', icon: 'logs'},
  {key: 'requestDebug', label: '请求调试', icon: 'requestDebug'},
  {key: 'usage', label: '用量统计', icon: 'memory'}
]

const currentView: Ref<string> = ref('dashboard')

// 主题切换
const theme: Ref<'light' | 'dark'> = ref('light')

const applyTheme = (): void => {
  document.documentElement.setAttribute('data-theme', theme.value)
  localStorage.setItem('theme', theme.value)
}

const initTheme = (): void => {
  const saved = localStorage.getItem('theme')
  if (saved === 'light' || saved === 'dark') {
    theme.value = saved
  } else {
    theme.value = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
  }
  applyTheme()
}

const toggleTheme = (): void => {
  theme.value = theme.value === 'dark' ? 'light' : 'dark'
  applyTheme()
}

initTheme()

// 全局 Toast
const {toast, toastError, showToast} = useToast()

// OneBot 配置
const qqConfig = reactive<QQConfigType>({
  accessToken: '',
  selfQQNumber: 0,
  qqHttpHost: '',
  botName: '小喵'
})

// Bot 头像（QQ 公开头像 CDN，失败时回退为线条图标）
const avatarUrl = computed<string>(() => {
  return qqConfig.selfQQNumber > 0
      ? `https://q1.qlogo.cn/g?b=qq&nk=${qqConfig.selfQQNumber}&s=100`
      : ''
})
const avatarFailed: Ref<boolean> = ref(false)

// WebSocket
const wsConnected: Ref<boolean> = ref(false)
let ws: WebSocket | null = null

const loadQQConfig = async (): Promise<void> => {
  const resp = await fetch('/admin/api/qq-config')
  const data = await resp.json()
  if (data.accessToken !== undefined) {
    Object.assign(qqConfig, data)
  }
}

const connectWebSocket = (): void => {
  const wsUrl = `ws://${location.host}/admin/ws`
  ws = new WebSocket(wsUrl)
  ws.onopen = () => wsConnected.value = true
  ws.onclose = () => {
    wsConnected.value = false
    setTimeout(connectWebSocket, 3000)
  }
}

// 提供给子组件
provide('showToast', showToast)
provide('qqConfig', qqConfig)
provide('wsConnected', wsConnected)
provide('ws', {get: () => ws})

onMounted(async () => {
  await loadQQConfig()
  connectWebSocket()
})
</script>

<template>
  <div class="container">
    <!-- 侧边栏 -->
    <div class="sidebar">
      <div class="sidebar-header">
        <div class="sidebar-logo">
          <div class="logo-icon">
            <img v-if="avatarUrl && !avatarFailed" :alt="qqConfig.botName" :src="avatarUrl"
                 @error="avatarFailed = true">
            <svg v-else fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"
                 stroke-width="2" viewBox="0 0 24 24">
              <path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/>
            </svg>
          </div>
          <div>
            <div class="sidebar-title">InSoulForge</div>
            <div class="sidebar-subtitle">管理后台</div>
          </div>
        </div>
      </div>

      <nav class="sidebar-nav">
        <div class="nav-section">
          <div
              v-for="item in dailyNavItems"
              :key="item.key"
              :class="{ active: currentView === item.key }"
              class="nav-item"
              @click="currentView = item.key"
          >
            <NavIcon :name="item.icon"/>
            {{ item.label }}
          </div>
        </div>

        <div class="nav-divider"></div>

        <div class="nav-section">
          <div
              v-for="item in systemNavItems"
              :key="item.key"
              :class="{ active: currentView === item.key }"
              class="nav-item"
              @click="currentView = item.key"
          >
            <NavIcon :name="item.icon"/>
            {{ item.label }}
          </div>
        </div>

        <div class="nav-divider"></div>

        <div class="nav-section">
          <div
              :class="{ active: currentView === 'about' }"
              class="nav-item"
              @click="currentView = 'about'"
          >
            <NavIcon name="info"/>
            关于
          </div>
        </div>
      </nav>

      <div class="sidebar-footer">
        <button class="theme-toggle" type="button" @click="toggleTheme">
          <!-- 月亮（当前深色模式） -->
          <svg v-if="theme === 'dark'" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
          </svg>
          <!-- 太阳（当前浅色模式） -->
          <svg v-else fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <circle cx="12" cy="12" r="5"/>
            <line x1="12" x2="12" y1="1" y2="3"/>
            <line x1="12" x2="12" y1="21" y2="23"/>
            <line x1="4.22" x2="5.64" y1="4.22" y2="5.64"/>
            <line x1="18.36" x2="19.78" y1="18.36" y2="19.78"/>
            <line x1="1" x2="3" y1="12" y2="12"/>
            <line x1="21" x2="23" y1="12" y2="12"/>
            <line x1="4.22" x2="5.64" y1="19.78" y2="18.36"/>
            <line x1="18.36" x2="19.78" y1="5.64" y2="4.22"/>
          </svg>
          {{ theme === 'dark' ? '深色模式' : '浅色模式' }}
        </button>
      </div>
    </div>

    <!-- 主内容区 -->
    <div class="main">
      <Dashboard v-if="currentView === 'dashboard'"/>
      <LLMConfig v-else-if="currentView === 'llm'"/>
      <PromptEditor v-else-if="currentView === 'prompts'"/>
      <CustomTools v-else-if="currentView === 'customTools'"/>
      <EmojiManager v-else-if="currentView === 'emojis'"/>
      <AdminManager v-else-if="currentView === 'admins'"/>
      <GroupManager v-else-if="currentView === 'groups'"/>
      <LogViewer v-else-if="currentView === 'logs'"/>
      <RequestDebug v-else-if="currentView === 'requestDebug'"/>
      <MemoryConfig v-else-if="currentView === 'memoryConfig'"/>
      <QQConfigVue v-else-if="currentView === 'qqConfig'"/>
      <UsageStats v-else-if="currentView === 'usage'"/>
      <About v-else-if="currentView === 'about'"/>
    </div>
  </div>

  <!-- Toast 提示 -->
  <div v-if="toast" :class="{ error: toastError }" class="toast">{{ toast }}</div>
</template>
