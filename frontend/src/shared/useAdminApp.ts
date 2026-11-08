import {computed, onMounted, onUnmounted, provide, reactive, ref, type Ref, watch} from 'vue'
import {confirmDiscard, type LeaveGuard} from '../composables/useUnsavedChanges'
import {useToast} from '../composables/useToast'
import {useLogoutConfirmation} from '../composables/useLogoutConfirmation'
import {useMobileNavigation} from '../composables/useMobileNavigation'
import type {QQConfig as QQConfigType} from '../vite-env.d'
import {DEFAULT_ACCENT, applyAccentColor, normalizeAccent, readAccentColor, saveAccentColor} from '../utils/theme'

export function useAdminApp() {


interface NavItem {
  key: string
  label: string
  icon: string
}


// 导航配置
const systemNavItems: NavItem[] = [
  {key: 'llm', label: '模型配置', icon: 'llm'},
  {key: 'executionConfig', label: '执行配置', icon: 'execution'},
  {key: 'qqConfig', label: 'OneBot 配置', icon: 'qq'},
  {key: 'prompts', label: '提示词', icon: 'prompts'},
  {key: 'customTools', label: '工具配置', icon: 'tool'},
  {key: 'mcpServers', label: 'MCP 服务', icon: 'tool'},
  {key: 'activities', label: 'Activity', icon: 'execution'},
  {key: 'memoryConfig', label: '记忆与上下文', icon: 'memory'}
]


const dailyNavItems: NavItem[] = [
  {key: 'dashboard', label: '首页', icon: 'home'},
  {key: 'emojis', label: '表情库', icon: 'emojis'},
  {key: 'admins', label: '访问管理', icon: 'admins'},
  {key: 'groups', label: '会话管理', icon: 'groups'},
  {key: 'people', label: '人物信息', icon: 'admins'},
  {key: 'logs', label: '运行日志', icon: 'logs'},
  {key: 'requestDebug', label: '请求调试', icon: 'requestDebug'},
  {key: 'system-tokens', label: '上下文占用', icon: 'prompts'},
  {key: 'usage', label: '用量统计', icon: 'memory'}
]


const leaveGuards = new Set<LeaveGuard>()

provide('leaveGuards', leaveGuards)

const canLeave = (): boolean => {
  if ([...leaveGuards].some(guard => guard.busy())) {
    showToast('正在保存，请等待完成后再离开', true)
    return false
  }
  return confirmDiscard([...leaveGuards].some(guard => guard.dirty()))
}

const {isMobile, mobileNavigationOpen} = useMobileNavigation(() => ![...leaveGuards].some(guard => guard.dirty() || guard.busy()))

const changeView = (view: string): boolean => {
  if (view !== currentView.value && !canLeave()) return false
  currentView.value = view
  mobileNavigationOpen.value = false
  return true
}

const currentView: Ref<string> = ref('dashboard')

const sidebarNav = ref<HTMLElement | null>(null)

const navIndicator = reactive({top: 0, height: 0})


function updateNavIndicator() {
  const item = sidebarNav.value?.querySelector<HTMLElement>('.nav-item.active')
  if (!item) {
    navIndicator.height = 0
    return
  }
  navIndicator.top = item.offsetTop
  navIndicator.height = item.offsetHeight
}


watch(currentView, updateNavIndicator, {flush: 'post'})

watch(sidebarNav, (nav, _, onCleanup) => {
  if (!nav) return
  updateNavIndicator()
  const observer = new ResizeObserver(updateNavIndicator)
  observer.observe(nav)
  onCleanup(() => observer.disconnect())
}, {flush: 'post'})


function resetPageScroll() {
  window.scrollTo({top: 0, behavior: 'instant'})
}

const authenticated: Ref<boolean> = ref(false)

watch(authenticated, () => { mobileNavigationOpen.value = false })

const authenticationChecked: Ref<boolean> = ref(false)

const accessToken: Ref<string> = ref('')

const loginError: Ref<string> = ref('')

const loginSubmitting: Ref<boolean> = ref(false)

const showAccessToken: Ref<boolean> = ref(false)


// 主题切换
const theme: Ref<'light' | 'dark'> = ref('light')

const accentColor = ref(DEFAULT_ACCENT)

const accentStorageError = ref('')

const setAccentColor = (value: string): void => {
  const color = normalizeAccent(value)
  if (!color) return
  accentColor.value = color
  applyAccentColor(document.documentElement, color, theme.value)
  let saved = false
  try { saved = saveAccentColor(localStorage, color) } catch { /* 浏览器可能禁止访问本地存储。 */ }
  accentStorageError.value = saved ? '' : '配色已生效，但当前浏览器无法保存；刷新后将恢复默认。'
}


const applyTheme = (): void => {
  document.documentElement.setAttribute('data-theme', theme.value)
  applyAccentColor(document.documentElement, accentColor.value, theme.value)
  try { localStorage.setItem('theme', theme.value) } catch { /* 仍允许本次使用主题。 */ }
}


const initTheme = (): void => {
  let saved: string | null = null
  try {
    saved = localStorage.getItem('theme')
    accentColor.value = readAccentColor(localStorage)
  } catch { /* 本地存储不可用时使用默认配色。 */ }
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
const {notifications, toastTarget, showToast, dismissToast} = useToast()


// OneBot 配置
const qqConfig = reactive<QQConfigType>({
  accessToken: '',
  selfQQNumber: '0',
  oneBotImplementation: 'napcat',
  oneBotTransport: 'http',
  qqHttpHost: '',
  qqWebSocketHost: '',
  botName: '机器人',
  defaultPrivateEnabled: false
})


// Bot 头像（QQ 公开头像 CDN，失败时回退为线条图标）
const avatarUrl = computed<string>(() => {
  return !!qqConfig.selfQQNumber && qqConfig.selfQQNumber !== '0'
      ? `https://q1.qlogo.cn/g?b=qq&nk=${qqConfig.selfQQNumber}&s=100`
      : ''
})

const avatarFailed: Ref<boolean> = ref(false)


// WebSocket
const wsConnected: Ref<boolean> = ref(false)

let ws: WebSocket | null = null

let reconnectTimer: number | undefined


const disconnectWebSocket = (): void => {
  window.clearTimeout(reconnectTimer)
  reconnectTimer = undefined
  if (ws) {
    ws.onclose = null
    ws.close()
    ws = null
  }
  wsConnected.value = false
}


const loadQQConfig = async (): Promise<void> => {
  const resp = await fetch('/admin/api/qq-config')
  const data = await resp.json()
  if (data.accessToken !== undefined) {
    Object.assign(qqConfig, data, {selfQQNumber: String(data.selfQQNumber ?? '0')})
  }
}


const connectWebSocket = (): void => {
  if (!authenticated.value || (ws && ws.readyState <= WebSocket.OPEN)) return
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
  const wsUrl = `${protocol}//${location.host}/admin/ws`
  ws = new WebSocket(wsUrl)
  ws.onopen = () => wsConnected.value = true
  ws.onclose = () => {
    wsConnected.value = false
    if (authenticated.value) {
      reconnectTimer = window.setTimeout(connectWebSocket, 3000)
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


const {visible: logoutConfirmationVisible, submitting: logoutSubmitting, error: logoutError, hasUnsavedChanges: logoutHasUnsavedChanges, request: logout, cancel: cancelLogout, confirm: confirmLogout} = useLogoutConfirmation(leaveGuards, () => {
  authenticated.value = false
  disconnectWebSocket()
}, () => showToast('正在保存，请等待完成后再退出登录', true))


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


onUnmounted(disconnectWebSocket)
return {systemNavItems, dailyNavItems, leaveGuards, canLeave, isMobile, mobileNavigationOpen, changeView, currentView, sidebarNav, navIndicator, updateNavIndicator, resetPageScroll, authenticated, authenticationChecked, accessToken, loginError, loginSubmitting, showAccessToken, theme, accentColor, accentStorageError, setAccentColor, applyTheme, initTheme, toggleTheme, themeToggleLabel, notifications, toastTarget, showToast, dismissToast, qqConfig, avatarUrl, avatarFailed, wsConnected, ws, reconnectTimer, disconnectWebSocket, loadQQConfig, connectWebSocket, initializeAdmin, checkAuthentication, loginWithToken, login, loginFromUrlFragment, logoutConfirmationVisible, logoutSubmitting, logoutError, logoutHasUnsavedChanges, logout, cancelLogout, confirmLogout}
}
