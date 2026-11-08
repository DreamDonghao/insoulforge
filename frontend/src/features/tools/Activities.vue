<script setup lang="ts">
import NavIcon from '../../components/NavIcon.vue'
import {computed, onBeforeUnmount, onMounted, ref} from 'vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'
import {useToast} from '../../composables/useToast'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import {requestError, requestJson} from '../../utils/api'

interface Service {
  name: string
  description: string
  enabled: boolean
  active: boolean
  instructions: string
  connected: boolean
  busy: boolean
  status: string
  updatedAt: number
}
interface Configuration {token: string; servers: Service[]}
const {showToast} = useToast()
const config = ref<Configuration>({token: '', servers: []})
const token = ref('')
const showToken = ref(false)
const loading = ref(true)
const loaded = ref(false)
const reading = ref(false)
const saving = ref(false)
const error = ref('')
let active = true
let timer: ReturnType<typeof setInterval> | undefined
const requests = new Set<AbortController>()
const dirty = computed(() => token.value !== config.value.token)
useUnsavedChanges(() => dirty.value, () => saving.value)

async function api<T = Configuration>(path = '', options?: RequestInit): Promise<T> {
  const controller = new AbortController()
  requests.add(controller)
  const timeout = setTimeout(() => controller.abort(), 15000)
  try {
    return await requestJson<T>(`/admin/api/activities${path}`, {...options, cache: 'no-store', signal: controller.signal})
  } finally {
    clearTimeout(timeout)
    requests.delete(controller)
  }
}
async function load(background = false): Promise<void> {
  if (reading.value || saving.value) return
  if (!background && !confirmDiscard(dirty.value)) return
  reading.value = true
  if (!background) loading.value = true
  try {
    const data = await api()
    if (!active) return
    if (!Array.isArray(data.servers) || typeof data.token !== 'string') throw new Error('Activity 配置数据无效')
    // 后台刷新只跟随未修改的 Token，不覆盖正在编辑的内容。
    if (!dirty.value || !background) token.value = data.token
    config.value = data
    loaded.value = true
    error.value = ''
  } catch (cause) {
    if (active) error.value = requestError(cause, '读取 Activity 配置失败')
  } finally {
    reading.value = false
    if (active) loading.value = false
  }
}
async function mutate(path: string, method: string, body?: object, successMessage = 'Activity 配置已保存'): Promise<void> {
  if (saving.value || reading.value || !loaded.value) return
  saving.value = true
  try {
    await api<{success: boolean}>(path, {method, headers: {'Content-Type': 'application/json'},
      ...(body ? {body: JSON.stringify(body)} : {})})
    if (active) showToast(successMessage)
  } catch (cause) {
    if (active) showToast(requestError(cause, '操作结果未确认，请刷新核对后再操作'), true)
  } finally {
    saving.value = false
  }
  if (active) await load(true)
}
async function saveToken(): Promise<void> {
  if (token.value && token.value.length < 16) {
    showToast('接入 Token 至少需要 16 个字符', true)
    return
  }
  await mutate('', 'POST', {token: token.value})
}
async function setEnabled(service: Service, enabled: boolean): Promise<void> {
  await mutate('', 'PATCH', {name: service.name, enabled})
}
function stateLabel(service: Service): string {
  if (!service.connected) return '工作状态未知'
  return service.active ? '正在工作' : '未工作'
}
async function remove(service: Service): Promise<void> {
  if (window.confirm(`删除 Activity“${service.name}”的接入记录？将断开连接并取消等待中的请求，不影响服务内部业务。服务重连后会重新登记。`))
    await mutate('', 'DELETE', {name: service.name})
}
const activeCount = computed(() => config.value.servers.filter(service => service.enabled && service.connected && service.active).length)
onMounted(() => {
  void load()
  timer = setInterval(() => { if (!document.hidden) void load(true) }, 5000)
})
onBeforeUnmount(() => {
  active = false
  clearInterval(timer)
  requests.forEach(controller => controller.abort())
})
</script>

<template>
  <div class="page-header">
    <div><h1 class="page-title">Activity</h1><p class="page-subtitle">服务自动注册，所有会话共享 · {{ activeCount }} 个正在工作</p></div>
    <button class="btn btn-secondary" :disabled="loading || reading || saving" @click="load()">刷新</button>
  </div>
  <div class="content activity-content">
    <p v-if="error" class="activity-error" role="alert">{{ error }}</p>
    <form class="token-form" @submit.prevent="saveToken">
      <div class="form-group">
        <label class="form-label" for="activity-token">Activity 服务接入 Token</label>
        <div class="secret-input">
          <input id="activity-token" v-model="token" class="form-input" :type="showToken ? 'text' : 'password'"
                 autocomplete="new-password" autocapitalize="off" :spellcheck="false" minlength="16"
                 aria-describedby="activity-token-hint" :disabled="!loaded || saving" placeholder="设置所有 Activity 服务连接时携带的 Token">
          <button type="button" class="secret-toggle" :disabled="!loaded || saving"
                  aria-controls="activity-token" :aria-pressed="showToken"
                  :title="showToken ? '隐藏接入 Token' : '显示接入 Token'"
                  :aria-label="showToken ? '隐藏接入 Token' : '显示接入 Token'" @click="showToken = !showToken">
            <NavIcon :name="showToken ? 'eye-off' : 'eye'"/>
          </button>
        </div>
        <p id="activity-token-hint" class="form-hint">至少 16 个字符。修改后已连接服务会断开，清空则关闭接入。服务连接 <code>/activities/ws</code>，携带此 Token 后自动上报名称和描述。</p>
      </div>
      <button class="btn btn-primary" :disabled="!loaded || reading || saving || !dirty">{{ saving ? '保存中…' : '保存 Token' }}</button>
    </form>
    <p v-if="loading">正在读取 Activity 服务…</p>
    <div v-else-if="!config.servers.length" class="empty-state">暂无 Activity 服务。设置接入 Token，再运行服务程序；连接注册后会自动出现在这里。</div>
    <div v-else class="activity-list">
      <article v-for="service in config.servers" :key="service.name" class="activity-row">
        <EnabledSwitch :model-value="service.enabled" :label="`${service.name}服务`"
                       :disabled="saving || reading" @update:model-value="setEnabled(service, $event)" />
        <div class="activity-info">
          <div class="activity-title"><h2>{{ service.name }}</h2>
            <span :class="{'is-active': service.active && service.connected}">{{ stateLabel(service) }}</span>
            <span>{{ service.connected ? '已连接' : '已断开' }}</span>
          </div>
          <p>{{ service.description || '暂无描述' }}</p>
          <p v-if="service.connected && service.status" class="activity-status">{{ service.status }}</p>
          <small v-if="service.updatedAt">最近状态：{{ new Date(service.updatedAt * 1000).toLocaleString() }}</small>
        </div>
        <div class="activity-actions">
          <button class="btn btn-danger" :disabled="saving || reading" @click="remove(service)">删除记录</button>
        </div>
      </article>
    </div>
    <p class="form-hint">状态由 Activity 主动上报；只有已启用、在线且正在工作的服务进入模型状态上下文。未工作的在线服务仍可调用。“刷新”和页面自动刷新只读取本地缓存。删除接入记录不会停止服务内部业务。</p>
  </div>
</template>

<style scoped>
.activity-content {display: flex; flex-direction: column; gap: 24px}
.page-header {grid-template-columns: minmax(0, 1fr) auto; gap: 16px}
.token-form {display: flex; align-items: flex-start; gap: 16px; border-bottom: 0; padding: 24px; background: var(--card-bg); border-radius: var(--radius-md); }
.token-form .form-group {flex: 1; min-width: 0; margin: 0}
.token-form > .btn {margin-top: 28px; flex-shrink: 0}
.activity-title {display: flex; align-items: center; gap: 12px; flex-wrap: wrap}
.activity-actions {display: flex; gap: 16px; flex-wrap: wrap; align-items: center}
.activity-title h2 {margin: 0; font-size: 18px; color: var(--text-primary)}
.activity-title span, small {font-size: 13px; color: var(--text-secondary)}
.activity-title .is-active {color: var(--primary)}
.activity-row {display: grid; grid-template-columns: 120px minmax(0, 1fr) auto; gap: 24px; padding: 24px; border-bottom: 0; align-items: start; background: var(--card-bg); margin-bottom: 16px; border-radius: var(--radius-md); }
.activity-info p {margin: 10px 0; overflow-wrap: anywhere; color: var(--text-secondary)}
.activity-info .activity-status {color: var(--text-primary)}
.activity-error {color: var(--danger) !important}
@media (max-width: 760px) {
  .activity-row {grid-template-columns: 100px minmax(0, 1fr); gap: 16px}
  .activity-row > .activity-actions {grid-column: 2; justify-self: start}
  .token-form {flex-direction: column}
  .token-form .form-group {width: 100%}
  .token-form > .btn {margin-top: 0}
}
@media (max-width: 440px) {.activity-row {grid-template-columns: 1fr} .activity-row > .activity-actions {grid-column: 1}}
</style>
