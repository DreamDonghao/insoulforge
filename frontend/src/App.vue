<script lang="ts" setup>
/**
 * @file App.vue
 * @brief 主应用组件 - 管理后台入口
 */
import {computed, onMounted, provide, reactive, ref, type Ref} from 'vue'
import {useToast} from './composables/useToast'
import type {QQConfig as QQConfigType} from './vite-env.d'

import NavIcon from './components/NavIcon.vue'
import Dashboard from './features/overview/Dashboard.vue'
import About from './features/overview/About.vue'
import AccessManager from './features/access/AccessManager.vue'
import GroupManager from './features/conversation/GroupManager.vue'
import MemoryConfig from './features/conversation/MemoryConfig.vue'
import LLMConfig from './features/llm/LLMConfig.vue'
import PromptEditor from './features/llm/PromptEditor.vue'
import UsageStats from './features/llm/UsageStats.vue'
import OneBotConfig from './features/onebot/OneBotConfig.vue'
import CustomTools from './features/tools/CustomTools.vue'
import EmojiManager from './features/tools/EmojiManager.vue'
import LogViewer from './features/diagnostics/LogViewer.vue'
import RequestDebug from './features/diagnostics/RequestDebug.vue'

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
  {key: 'admins', label: '访问管理', icon: 'admins'},
  {key: 'groups', label: '会话管理', icon: 'groups'},
  {key: 'logs', label: '运行日志', icon: 'logs'},
  {key: 'requestDebug', label: '请求调试', icon: 'requestDebug'},
  {key: 'usage', label: '用量统计', icon: 'memory'}
]

const currentView: Ref<string> = ref('dashboard')
const authenticated: Ref<boolean> = ref(false)
const authenticationChecked: Ref<boolean> = ref(false)
const accessToken: Ref<string> = ref('')
const loginError: Ref<string> = ref('')
const loginSubmitting: Ref<boolean> = ref(false)
const showAccessToken: Ref<boolean> = ref(false)

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

const themeToggleLabel = computed<string>(() => {
  return theme.value === 'dark' ? '切换为浅色模式' : '切换为深色模式'
})

initTheme()

// 全局 Toast
const {toast, toastError, showToast} = useToast()

// OneBot 配置
const qqConfig = reactive<QQConfigType>({
  accessToken: '',
  selfQQNumber: 0,
  oneBotTransport: 'http',
  qqHttpHost: '',
  qqWebSocketHost: '',
  botName: '机器人'
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
    if (authenticated.value) {
      setTimeout(connectWebSocket, 3000)
    }
  }
}

const initializeAdmin = async (): Promise<void> => {
  await loadQQConfig()
  connectWebSocket()
}

const checkAuthentication = async (): Promise<void> => {
  try {
    const response = await fetch('/admin/api/auth/status')
    const data = await response.json()
    authenticated.value = data.authenticated === true
  } catch {
    authenticated.value = false
  } finally {
    authenticationChecked.value = true
  }
}

const loginWithToken = async (token: string): Promise<boolean> => {
  const response = await fetch('/admin/api/auth/login', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({token})
  })
  if (!response.ok) {
    return false
  }
  authenticated.value = true
  return true
}

const login = async (): Promise<void> => {
  loginError.value = ''
  loginSubmitting.value = true
  try {
    if (!await loginWithToken(accessToken.value)) {
      loginError.value = '访问令牌无效'
      return
    }
    accessToken.value = ''
    await initializeAdmin()
  } catch {
    loginError.value = '无法连接管理服务'
  } finally {
    loginSubmitting.value = false
  }
}

const loginFromUrlFragment = async (): Promise<void> => {
  const token = new URLSearchParams(location.hash.slice(1)).get('token')
  if (!token) {
    return
  }

  // URL 片段不会发送给服务端，仍需在请求前清除，避免令牌留在浏览器历史记录中。
  history.replaceState(null, '', `${location.pathname}${location.search}`)
  try {
    if (!await loginWithToken(token)) {
      loginError.value = '自动登录链接已失效'
    }
  } catch {
    loginError.value = '无法连接管理服务'
  }
}

const logout = async (): Promise<void> => {
  await fetch('/admin/api/auth/logout', {method: 'POST'})
  ws?.close()
  ws = null
  authenticated.value = false
}

// 提供给子组件
provide('showToast', showToast)
provide('qqConfig', qqConfig)
provide('wsConnected', wsConnected)
provide('ws', {get: () => ws})

onMounted(async () => {
  await checkAuthentication()
  if (!authenticated.value) {
    await loginFromUrlFragment()
  }
  if (authenticated.value) {
    await initializeAdmin()
  }
})
</script>

<template>
  <main v-if="!authenticationChecked" class="login-page">
    <button :aria-label="themeToggleLabel" :title="themeToggleLabel" class="login-theme-toggle" type="button"
            @click="toggleTheme">
      <svg v-if="theme === 'dark'" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path
            d="M12 3v2m0 14v2M3 12h2m14 0h2M5.64 5.64l1.42 1.42m9.88 9.88 1.42 1.42m0-12.72-1.42 1.42m-9.88 9.88-1.42 1.42"/>
        <circle cx="12" cy="12" r="4"/>
      </svg>
      <svg v-else fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
      </svg>
      <span>{{ theme === 'dark' ? '深色模式' : '浅色模式' }}</span>
    </button>
    <div class="login-shell">
      <div class="login-identity">
        <div aria-hidden="true" class="login-logo">
          <svg fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
               viewBox="0 0 24 24">
            <path d="M12 2 2 7l10 5 10-5-10-5Z"/>
            <path d="m2 12 10 5 10-5M2 17l10 5 10-5"/>
          </svg>
        </div>
        <div>
          <div class="login-product-name">InSoulForge</div>
          <div class="login-product-type">管理后台</div>
        </div>
      </div>
      <div aria-live="polite" class="login-panel login-status">
        <span aria-hidden="true" class="loading-indicator"></span>
        正在验证登录状态...
      </div>
    </div>
  </main>

  <main v-else-if="!authenticated" class="login-page">
    <button :aria-label="themeToggleLabel" :title="themeToggleLabel" class="login-theme-toggle" type="button"
            @click="toggleTheme">
      <svg v-if="theme === 'dark'" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path
            d="M12 3v2m0 14v2M3 12h2m14 0h2M5.64 5.64l1.42 1.42m9.88 9.88 1.42 1.42m0-12.72-1.42 1.42m-9.88 9.88-1.42 1.42"/>
        <circle cx="12" cy="12" r="4"/>
      </svg>
      <svg v-else fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
      </svg>
      <span>{{ theme === 'dark' ? '深色模式' : '浅色模式' }}</span>
    </button>
    <div class="login-shell">
      <div class="login-identity">
        <div aria-hidden="true" class="login-logo">
          <svg fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
               viewBox="0 0 24 24">
            <path d="M12 2 2 7l10 5 10-5-10-5Z"/>
            <path d="m2 12 10 5 10-5M2 17l10 5 10-5"/>
          </svg>
        </div>
        <div>
          <div class="login-product-name">InSoulForge</div>
          <div class="login-product-type">管理后台</div>
        </div>
      </div>
      <form class="login-panel" @submit.prevent="login">
        <div class="login-eyebrow">安全访问</div>
        <h1>登录管理后台</h1>
        <p>请输入服务启动日志中生成的访问令牌。</p>
        <label class="login-label" for="access-token">访问令牌</label>
        <div class="login-input-wrap">
          <input id="access-token" v-model="accessToken" :disabled="loginSubmitting"
                 :type="showAccessToken ? 'text' : 'password'" autocomplete="current-password"
                 autofocus class="login-input" spellcheck="false">
          <button :aria-label="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'"
                  :title="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'" class="input-icon-button" type="button"
                  @click="showAccessToken = !showAccessToken">
            <svg v-if="showAccessToken" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path
                  d="m3 3 18 18M10.58 10.58a2 2 0 0 0 2.83 2.83M9.88 4.24A10.94 10.94 0 0 1 12 4c5 0 9.27 3.11 11 7.5a11.82 11.82 0 0 1-2.08 3.19M6.61 6.61A11.87 11.87 0 0 0 1 11.5C2.73 15.89 7 19 12 19c1.61 0 3.14-.32 4.53-.9"/>
            </svg>
            <svg v-else fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/>
              <circle cx="12" cy="12" r="3"/>
            </svg>
          </button>
        </div>
        <div v-if="loginError" class="login-error" role="alert">{{ loginError }}</div>
        <button :disabled="loginSubmitting || !accessToken" class="login-button" type="submit">
          {{ loginSubmitting ? '验证中...' : '进入后台' }}
        </button>
      </form>
    </div>
  </main>

  <div v-else class="container">
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
        <div class="sidebar-actions">
          <button :aria-label="themeToggleLabel" :title="themeToggleLabel" class="sidebar-action" type="button"
                  @click="toggleTheme">
            <svg v-if="theme === 'dark'" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
            </svg>
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
            <span>{{ theme === 'dark' ? '深色模式' : '浅色模式' }}</span>
          </button>
          <button class="sidebar-action sidebar-action-danger" type="button" @click="logout">
            <svg fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                 viewBox="0 0 24 24">
              <path d="M10 17l5-5-5-5M15 12H3M21 19V5a2 2 0 0 0-2-2h-6"/>
            </svg>
            <span>退出登录</span>
          </button>
        </div>
      </div>
    </div>

    <!-- 主内容区 -->
    <div class="main">
      <Dashboard v-if="currentView === 'dashboard'"/>
      <LLMConfig v-else-if="currentView === 'llm'"/>
      <PromptEditor v-else-if="currentView === 'prompts'"/>
      <CustomTools v-else-if="currentView === 'customTools'"/>
      <EmojiManager v-else-if="currentView === 'emojis'"/>
      <AccessManager v-else-if="currentView === 'admins'"/>
      <GroupManager v-else-if="currentView === 'groups'"/>
      <LogViewer v-else-if="currentView === 'logs'"/>
      <RequestDebug v-else-if="currentView === 'requestDebug'"/>
      <MemoryConfig v-else-if="currentView === 'memoryConfig'"/>
      <OneBotConfig v-else-if="currentView === 'qqConfig'"/>
      <UsageStats v-else-if="currentView === 'usage'"/>
      <About v-else-if="currentView === 'about'"/>
    </div>
  </div>

  <!-- Toast 提示 -->
  <div v-if="toast" :class="{ error: toastError }" class="toast">{{ toast }}</div>
</template>

<style>
.login-page {
  position: relative;
  display: flex;
  flex-direction: column;
  min-height: 100vh;
  align-items: center;
  justify-content: center;
  padding: 32px 24px;
  background: var(--bg-main);
}

.login-theme-toggle {
  position: fixed;
  top: 24px;
  right: 24px;
  display: flex;
  align-items: center;
  gap: 8px;
  height: 36px;
  padding: 0 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--card-bg);
  color: var(--text-secondary);
  cursor: pointer;
  font: inherit;
  font-size: 13px;
  font-weight: 600;
  box-shadow: var(--shadow-sm);
}

.login-theme-toggle:hover {
  border-color: var(--border-strong);
  background: var(--row-hover);
  color: var(--text-primary);
}

.login-theme-toggle svg {
  width: 16px;
  height: 16px;
}

.login-shell {
  width: min(100%, 420px);
}

.login-identity {
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 0 0 24px 4px;
}

.login-logo {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 42px;
  height: 42px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--card-bg);
  color: var(--primary);
  box-shadow: var(--shadow-sm);
}

.login-logo svg {
  width: 23px;
  height: 23px;
}

.login-product-name {
  color: var(--text-primary);
  font-size: 18px;
  font-weight: 700;
}

.login-product-type {
  margin-top: 1px;
  color: var(--text-light);
  font-size: 12px;
  font-weight: 500;
}

.login-panel {
  padding: 28px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--card-bg);
  box-shadow: var(--shadow-sm);
}

.login-eyebrow {
  margin-bottom: 6px;
  color: var(--primary);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.8px;
  text-transform: uppercase;
}

.login-panel h1 {
  margin-bottom: 8px;
  font-size: 22px;
  letter-spacing: 0;
}

.login-panel p {
  margin-bottom: 22px;
  color: var(--text-secondary);
  font-size: 14px;
}

.login-label {
  display: block;
  margin-bottom: 8px;
  font-weight: 600;
}

.login-input {
  width: 100%;
  height: 42px;
  padding: 0 42px 0 12px;
  border: 1px solid var(--border-strong);
  border-radius: 6px;
  outline: none;
  background: var(--input-bg);
  color: var(--text-primary);
}

.login-input-wrap {
  position: relative;
}

.input-icon-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  position: absolute;
  top: 5px;
  right: 5px;
  width: 32px;
  height: 32px;
  border: 0;
  border-radius: 5px;
  background: transparent;
  color: var(--text-secondary);
  cursor: pointer;
}

.input-icon-button:hover {
  background: var(--row-hover);
  color: var(--text-primary);
}

.input-icon-button svg {
  width: 17px;
  height: 17px;
}

.login-input:focus {
  border-color: var(--primary);
  box-shadow: 0 0 0 3px var(--focus-ring);
}

.login-error {
  margin-top: 12px;
  padding: 8px 10px;
  border-radius: 5px;
  background: var(--danger-soft);
  color: var(--danger);
  font-size: 13px;
}

.login-button {
  border-radius: 6px;
  border: 0;
  cursor: pointer;
  font: inherit;
}

.login-button {
  width: 100%;
  height: 42px;
  margin-top: 22px;
  background: var(--primary);
  color: var(--on-primary);
  font-weight: 600;
  transition: background-color 0.15s ease, transform 0.15s ease;
}

.login-button:not(:disabled):hover {
  background: var(--primary-hover);
}

.login-button:not(:disabled):active {
  transform: translateY(1px);
}

.login-button:disabled {
  cursor: not-allowed;
  opacity: 0.55;
}

.loading-indicator {
  width: 16px;
  height: 16px;
  border: 2px solid var(--border-strong);
  border-top-color: var(--primary);
  border-radius: 50%;
  animation: login-spin 0.8s linear infinite;
}

.login-status {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
  min-height: 96px;
  color: var(--text-secondary);
}

@keyframes login-spin {
  to {
    transform: rotate(360deg);
  }
}

@media (max-width: 600px) {
  .login-page {
    padding: 24px 16px;
  }

  .login-theme-toggle {
    top: 16px;
    right: 16px;
  }

  .login-panel {
    padding: 24px;
  }
}
</style>
