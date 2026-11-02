<script setup lang="ts">
import {computed, onBeforeUnmount, onMounted, reactive, ref} from 'vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'
import ModalDialog from '../../components/ModalDialog.vue'
import {useToast} from '../../composables/useToast'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import {requestError, requestJson} from '../../utils/api'

interface ServiceDefinition {
  name: string
  description: string
  url: string
  enabled: boolean
  token?: string
}
interface Service extends ServiceDefinition {
  running?: boolean
  reachable?: boolean
  synchronized?: boolean
  busy?: boolean
  status?: string
  updatedAt?: number
}
interface Configuration {callbackUrl: string; servers: Service[]}
const {showToast} = useToast()
const config = ref<Configuration>({callbackUrl: '', servers: []})
const callbackUrl = ref('')
const loading = ref(true)
const loaded = ref(false)
const saving = ref(false)
const error = ref('')
const dialog = ref(false)
const showToken = ref(false)
const editing = ref<string | null>(null)
const draft = reactive<ServiceDefinition>({name: '', description: '', url: '', enabled: true, token: ''})
let initialDraft = ''
let active = true
const reading = ref(false)
let timer: ReturnType<typeof setInterval> | undefined
const requests = new Set<AbortController>()
const dirty = computed(() => callbackUrl.value !== config.value.callbackUrl ||
  (dialog.value && JSON.stringify(draft) !== initialDraft))
useUnsavedChanges(() => dirty.value, () => saving.value)

async function api<T = Configuration>(options?: RequestInit): Promise<T> {
  const controller = new AbortController()
  requests.add(controller)
  const timeout = setTimeout(() => controller.abort(), 15000)
  try {
    return await requestJson<T>('/admin/api/activities', {...options, cache: 'no-store', signal: controller.signal})
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
    if (!Array.isArray(data.servers) || typeof data.callbackUrl !== 'string') throw new Error('活动配置数据无效')
    // 后台轮询只更新已确认配置，不覆盖编辑中的回调地址和弹窗草稿。
    if (callbackUrl.value === config.value.callbackUrl || !background) callbackUrl.value = data.callbackUrl
    config.value = data
    loaded.value = true
    error.value = ''
  } catch (cause) {
    if (active) error.value = requestError(cause, '读取活动配置失败')
  } finally {
    reading.value = false
    if (active) loading.value = false
  }
}
function locked(service: Service): boolean {
  return !!service.running || !!service.busy
}
function open(service?: Service): void {
  showToken.value = false
  editing.value = service?.name ?? null
  Object.assign(draft, {name: service?.name ?? '', description: service?.description ?? '',
    url: service?.url ?? '', enabled: service?.enabled ?? true, token: service?.token ?? ''})
  initialDraft = JSON.stringify(draft)
  dialog.value = true
}
function close(): void {
  if (!saving.value && confirmDiscard(JSON.stringify(draft) !== initialDraft)) {
    dialog.value = false
    showToken.value = false
  }
}
function definition(service: ServiceDefinition): ServiceDefinition {
  return {name: service.name, description: service.description, url: service.url, enabled: service.enabled,
    ...(service.token ? {token: service.token} : {})}
}
async function persist(servers: ServiceDefinition[], closeDialog = false): Promise<void> {
  if (saving.value || reading.value || !loaded.value) return
  saving.value = true
  try {
    await api<{success: boolean}>({method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({callbackUrl: callbackUrl.value, servers: servers.map(definition)})})
    if (!active) return
    if (closeDialog) {
      dialog.value = false
      showToken.value = false
    }
    showToast('活动配置已保存')
  } catch (cause) {
    if (active) showToast(requestError(cause, '保存结果未确认，请刷新核对后再操作'), true)
  } finally {
    saving.value = false
  }
  if (active) await load(true)
}
async function saveService(): Promise<void> {
  const servers = config.value.servers.filter(server => server.name !== editing.value)
  if (servers.some(server => server.name === draft.name)) {
    showToast('活动名称已经存在', true)
    return
  }
  if (!editing.value && (draft.token?.length ?? 0) < 16) {
    showToast('共享密钥至少需要 16 个字符', true)
    return
  }
  await persist([...servers, draft], true)
}
async function remove(service: Service): Promise<void> {
  if (window.confirm(`删除活动服务“${service.name}”？`))
    await persist(config.value.servers.filter(item => item.name !== service.name))
}
const runningCount = computed(() => config.value.servers.filter(service => service.running).length)
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
    <div><h1 class="page-title">实时活动</h1><p class="page-subtitle">跨会话共享、持续交互的活动 · {{ runningCount }} 个进行中</p></div>
    <div class="activity-actions">
      <button class="btn btn-secondary" :disabled="loading || saving" @click="load()">刷新</button>
      <button class="btn btn-primary" :disabled="!loaded || reading || saving" @click="open()">添加服务</button>
    </div>
  </div>
  <div class="content activity-content">
    <p v-if="error" class="activity-error" role="alert">{{ error }}</p>
    <form class="callback-form" @submit.prevent="persist(config.servers)">
      <div class="form-group">
        <label class="form-label" for="activity-callback">事件回调地址</label>
        <input id="activity-callback" v-model="callbackUrl" class="form-input" type="url"
               placeholder="http://insoulforge:7778/activities/events" :disabled="saving || loading">
        <p class="form-hint">填写活动服务能访问的完整地址。运行期间不能修改，服务使用自己的共享密钥上报。</p>
      </div>
      <button class="btn btn-secondary" :disabled="!loaded || reading || saving || callbackUrl === config.callbackUrl">保存地址</button>
    </form>
    <p v-if="loading">正在读取活动服务…</p>
    <div v-else-if="!config.servers.length" class="empty-state">暂无活动服务，添加后模型即可通过工具启动和指挥活动。</div>
    <div v-else class="activity-list">
      <article v-for="service in config.servers" :key="service.name" class="activity-row">
        <EnabledSwitch :model-value="service.enabled" :label="`${service.name}服务`"
                       :disabled="saving || reading || locked(service)" @update:model-value="persist(config.servers.map(item => item.name === service.name ? {...item, enabled: $event} : item))" />
        <div class="activity-info">
          <div class="activity-title"><h2>{{ service.name }}</h2>
            <span :class="{'is-running': service.running}">{{ service.running ? '活动运行中' : !service.enabled ? '服务未启用' : service.synchronized ? '当前无活动' : '等待状态同步' }}</span>
            <span v-if="service.enabled && !service.reachable" class="activity-error">状态未确认</span>
          </div>
          <p>{{ service.description || '暂无描述' }}</p>
          <p class="activity-url">{{ service.url }}</p>
          <p v-if="service.running" class="activity-status">{{ service.status }}</p>
          <small v-if="service.updatedAt">最近状态：{{ new Date(service.updatedAt * 1000).toLocaleString() }}</small>
        </div>
        <div class="activity-actions">
          <button class="btn btn-secondary" :disabled="saving || reading || locked(service)" @click="open(service)">编辑</button>
          <button class="btn btn-danger" :disabled="saving || reading || locked(service)" @click="remove(service)">删除</button>
        </div>
      </article>
    </div>
    <p class="form-hint">暂停、恢复和结束活动由模型调用工具完成。已知运行中的服务保留配置，防止丢失活动。</p>
  </div>
  <ModalDialog v-if="dialog" class="activity-overlay" labelledby="activity-title" @close="close">
    <form class="activity-dialog" @submit.prevent="saveService">
      <header class="activity-dialog-header"><h2 id="activity-title">{{ editing ? '编辑活动服务' : '添加活动服务' }}</h2>
        <button type="button" class="btn btn-secondary activity-close" aria-label="关闭" :disabled="saving" @click="close">×</button></header>
      <div class="activity-dialog-body">
        <div class="form-group"><label class="form-label" for="activity-name">活动名称</label>
          <input id="activity-name" v-model="draft.name" class="form-input" required maxlength="80" :disabled="saving || !!editing" autofocus></div>
        <div class="form-group"><label class="form-label" for="activity-url">服务地址</label>
          <input id="activity-url" v-model="draft.url" class="form-input" type="url" required :disabled="saving" placeholder="http://127.0.0.1:8090"></div>
        <div class="form-group"><label class="form-label" for="activity-description">用途描述（选填）</label>
          <textarea id="activity-description" v-model="draft.description" class="form-input" rows="3" maxlength="4096" :disabled="saving" placeholder="说明活动的用途，帮助模型决定何时启动"></textarea></div>
        <div class="form-group"><label class="form-label" for="activity-token">共享密钥</label>
          <div class="activity-secret">
            <input id="activity-token" v-model="draft.token" class="form-input" :type="showToken ? 'text' : 'password'"
                   autocomplete="new-password" autocapitalize="off" :spellcheck="false" aria-describedby="activity-token-hint"
                   :required="!editing" minlength="16" :disabled="saving" :placeholder="editing ? '留空沿用已有密钥' : '输入与活动服务一致的共享密钥'">
            <button type="button" class="btn btn-secondary" :disabled="saving"
                    aria-controls="activity-token" :aria-pressed="showToken"
                    :aria-label="showToken ? '隐藏共享密钥' : '显示共享密钥'" @click="showToken = !showToken">
              {{ showToken ? '隐藏' : '显示' }}
            </button>
          </div>
          <p id="activity-token-hint" class="form-hint">{{ editing ? '已载入当前密钥，点击“显示”可查看；修改后随表单保存，至少需要 16 个字符。' : '必填，至少 16 个字符，必须与活动服务配置一致。' }}</p>
        </div>
        <label class="form-checkbox"><input v-model="draft.enabled" type="checkbox" :disabled="saving">启用活动服务（保存后生效）</label>
      </div>
      <footer class="activity-actions dialog-footer">
        <button type="button" class="btn btn-secondary" :disabled="saving" @click="close">取消</button>
        <button class="btn btn-primary" :disabled="saving || reading">{{ saving ? '保存中…' : '保存服务' }}</button>
      </footer>
    </form>
  </ModalDialog>
</template>

<style scoped>
.activity-content {display: flex; flex-direction: column; gap: 24px}
.page-header {grid-template-columns: minmax(0, 1fr) auto; gap: 16px}
.activity-actions, .activity-title {display: flex; align-items: center; gap: 12px; flex-wrap: wrap}
.activity-title h2 {margin: 0; font-size: 18px; color: var(--text-primary)}
.activity-title span, small {font-size: 13px; color: var(--text-secondary)}
.activity-title .is-running {color: var(--primary)}
.callback-form {display: flex; align-items: flex-start; gap: 16px; border-bottom: 1px solid var(--border-color); padding-bottom: 24px}
.callback-form .form-group {flex: 1; margin: 0; min-width: 0}
.callback-form > button {margin-top: 28px; flex-shrink: 0}
.activity-row {display: grid; grid-template-columns: 120px minmax(0, 1fr) auto; gap: 24px; padding: 28px 0; border-bottom: 1px solid var(--border-color); align-items: start}
.activity-info p {margin: 10px 0; overflow-wrap: anywhere; color: var(--text-secondary)}
.activity-info .activity-status {color: var(--text-primary)}
.activity-url {font-size: 13px}
.activity-error {color: var(--danger) !important}
.activity-overlay {position: fixed; inset: 0; z-index: 1000; display: grid; place-items: center;
  padding: 24px; box-sizing: border-box; background: rgba(0, 0, 0, .5)}
.activity-dialog {width: min(560px, 100%); max-height: calc(100dvh - 48px); min-width: 0;
  display: flex; flex-direction: column; overflow: hidden; background: var(--card-bg); border: 1px solid var(--border)}
.activity-dialog-header {display: flex; align-items: center; justify-content: space-between; gap: 16px;
  padding: 18px 24px; border-bottom: 1px solid var(--border); flex-shrink: 0}
.activity-dialog-header h2 {margin: 0; font-size: 18px}
.activity-close {width: 32px; height: 32px; padding: 0; font-size: 20px; flex-shrink: 0}
.activity-dialog-body {padding: 24px; min-height: 0; overflow-y: auto}
.activity-dialog textarea.form-input {flex: none; min-height: 96px; height: 112px; max-height: 220px; font-family: inherit}
.activity-secret {display: flex; align-items: stretch; gap: 8px}
.activity-secret .form-input {flex: 1; min-width: 0}
.activity-secret .btn {flex-shrink: 0; min-width: 64px}
.dialog-footer {justify-content: flex-end; padding: 16px 24px; border-top: 1px solid var(--border); flex-shrink: 0}
@media (max-width: 760px) {
  .activity-row {grid-template-columns: 100px minmax(0, 1fr); gap: 16px}
  .activity-row > .activity-actions {grid-column: 2}
  .callback-form {flex-direction: column}
  .callback-form .form-group {width: 100%}
  .callback-form > button {margin-top: 0}
}
@media (max-width: 440px) {.activity-row {grid-template-columns: 1fr} .activity-row > .activity-actions {grid-column: 1}}
@media (max-width: 600px) {
  .activity-overlay {padding: 12px}
  .activity-dialog {max-height: calc(100dvh - 24px)}
  .activity-dialog-header, .activity-dialog-body, .dialog-footer {padding: 16px}
}
</style>
