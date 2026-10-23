<script setup lang="ts">
import {computed, inject, nextTick, onMounted, onUnmounted, reactive, ref, watch} from 'vue'
import NavIcon from '../../components/NavIcon.vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'

interface Server {
  name: string
  description: string
  url: string
  token: string
  headers: Record<string, string>
  enabled: boolean
  timeoutSeconds: number
}

interface Status {
  name: string
  loaded: boolean
  loading: boolean
  toolCount: number
  tools?: {name: string; description: string}[]
  error: string
}

const toast = inject<(message: string, error?: boolean) => void>('showToast')
const servers = ref<Server[]>([])
const originalServers = ref<unknown[]>([])
const statuses = ref<Status[]>([])
const loading = ref(true)
const busy = ref(false)
const error = ref('')
const editing = ref<number | null>(null)
const dialog = ref(false)
const dialogElement = ref<HTMLFormElement | null>(null)
const draft = reactive<Server>(emptyServer())
const headerRows = ref<{name: string; value: string}[]>([])
const validHeaders = computed(() => {
  const names = headerRows.value.map(header => header.name.trim().toLowerCase())
  return names.every(name => /^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) &&
      new Set(names).size === names.length &&
      headerRows.value.every(header => !/[\r\n\0]/.test(header.value))
})
const descriptionBytes = computed(() => new TextEncoder().encode(draft.description).length)
const statusByName = computed(() => new Map(statuses.value.map(status => [status.name, status])))
const valid = computed(() => /^[A-Za-z0-9_-]{1,20}$/.test(draft.name.trim()) &&
    validHeaders.value &&
    descriptionBytes.value <= 1024 &&
    validUrl(draft.url.trim()) &&
    Number.isFinite(draft.timeoutSeconds) && draft.timeoutSeconds >= 1 && draft.timeoutSeconds <= 120 &&
    !servers.value.some((server, index) => index !== editing.value && server.name === draft.name.trim()))

const requests = new Set<AbortController>()
let active = true
let reading = false
let loadVersion = 0
let timer: ReturnType<typeof setInterval> | undefined
let previousFocus: HTMLElement | null = null
let previousOverflow = ''

function emptyServer(): Server {
  return {name: '', description: '', url: '', token: '', headers: {}, enabled: true, timeoutSeconds: 30}
}

function validUrl(value: string): boolean {
  try {
    const url = new URL(value)
    return /^https?:\/\//.test(value) && !!url.hostname &&
        !url.username && !url.password && !value.includes('#') && !/[\s\\]/.test(value)
  } catch {
    return false
  }
}

function statusText(server: Server): string {
  if (!server.enabled) return '已禁用'
  const status = statusByName.value.get(server.name)
  if (status?.loading) return '加载中'
  if (status?.error) return status.error
  return status?.loaded ? `已加载 ${status.toolCount} 个工具` : '等待加载'
}

function errorMessage(cause: unknown): string {
  return cause instanceof Error ? cause.message : '无法连接管理服务'
}

async function request(url: string, options: RequestInit = {}): Promise<Record<string, unknown>> {
  const controller = new AbortController()
  requests.add(controller)
  try {
    const response = await fetch(url, {...options, cache: 'no-store', signal: controller.signal})
    const result: unknown = await response.json()
    if (!result || typeof result !== 'object' || Array.isArray(result)) throw new Error('管理接口返回数据无效')
    const data = result as Record<string, unknown>
    if (!response.ok) throw new Error(typeof data.error === 'string' ? data.error : `请求失败（HTTP ${response.status}）`)
    return data
  } finally {
    requests.delete(controller)
  }
}

async function load(background = false): Promise<void> {
  if (reading) return
  reading = true
  const version = ++loadVersion
  if (!background) {
    loading.value = true
    error.value = ''
  }
  try {
    const data = await request('/admin/api/mcp-config')
    if (!Array.isArray(data.servers) || !Array.isArray(data.status)) throw new Error('MCP 配置数据无效')
    if (!active || version !== loadVersion) return
    if (!background) {
      servers.value = data.servers.map((value: unknown): Server => {
        if (!value || typeof value !== 'object') throw new Error('MCP 服务数据无效')
        const server = value as Partial<Server>
        if (typeof server.name !== 'string' || typeof server.url !== 'string') throw new Error('MCP 服务数据无效')
        if ((server.token !== undefined && typeof server.token !== 'string') ||
            (server.headers !== undefined && (!server.headers || typeof server.headers !== 'object' ||
                Array.isArray(server.headers) || Object.values(server.headers).some(value => typeof value !== 'string'))) ||
            (server.description !== undefined && typeof server.description !== 'string') ||
            (server.enabled !== undefined && typeof server.enabled !== 'boolean') ||
            (server.timeoutSeconds !== undefined && typeof server.timeoutSeconds !== 'number')) {
          throw new Error('MCP 服务字段类型无效')
        }
        return {
          name: server.name, url: server.url, token: server.token ?? '',
          headers: server.headers ?? {},
          description: server.description ?? '',
          enabled: server.enabled ?? true, timeoutSeconds: server.timeoutSeconds ?? 30
        }
      })
      originalServers.value = data.servers
    }
    statuses.value = data.status as Status[]
  } catch (cause) {
    if (!background && active && version === loadVersion) error.value = errorMessage(cause)
  } finally {
    reading = false
    if (!background && active) loading.value = false
  }
}

function open(index: number | null): void {
  previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
  editing.value = index
  Object.assign(draft, index === null ? emptyServer() : servers.value[index])
  headerRows.value = Object.entries(draft.headers).map(([name, value]) => ({name, value}))
  dialog.value = true
}

function handleDialogKey(event: KeyboardEvent): void {
  if (event.key === 'Escape' && !busy.value) dialog.value = false
  if (event.key !== 'Tab') return
  const controls = dialogElement.value?.querySelectorAll<HTMLElement>('input:not(:disabled), textarea:not(:disabled), button:not(:disabled)')
  if (!controls?.length) {
    event.preventDefault()
    return
  }
  const first = controls[0]!
  const last = controls[controls.length - 1]!
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault()
    last.focus()
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault()
    first.focus()
  }
}

watch(dialog, async visible => {
  if (visible) {
    previousOverflow = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    await nextTick()
    if (active && dialog.value) dialogElement.value?.querySelector<HTMLInputElement>('input')?.focus()
  } else {
    document.body.style.overflow = previousOverflow
    previousFocus?.focus()
  }
})

async function persist(next: Server[]): Promise<boolean> {
  if (busy.value) return false
  busy.value = true
  ++loadVersion // 保存之前发出的轮询结果不能覆盖保存后的状态。
  try {
    const result = await request('/admin/api/mcp-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({servers: next, originalServers: originalServers.value})
    })
    if (result.success !== true) throw new Error(typeof result.error === 'string' ? result.error : '保存失败')
    if (active) {
      servers.value = next
      originalServers.value = next
      toast?.('MCP 服务已更新')
      await load(true)
    }
    return true
  } catch (cause) {
    if (active) toast?.(errorMessage(cause), true)
    return false
  } finally {
    busy.value = false
  }
}

async function save(): Promise<void> {
  if (busy.value || !valid.value) return
  const next = servers.value.map(server => ({...server}))
  const value = {...draft, name: draft.name.trim(), url: draft.url.trim(),
    headers: Object.fromEntries(headerRows.value.map(header => [header.name.trim(), header.value]))}
  if (editing.value === null) next.push(value)
  else next[editing.value] = value
  if (await persist(next)) dialog.value = false
}

async function remove(index: number): Promise<void> {
  const server = servers.value[index]
  if (!server || busy.value || !window.confirm(`删除 MCP 服务“${server.name}”？`)) return
  await persist(servers.value.filter((_, position) => position !== index))
}

async function toggle(index: number, enabled: boolean): Promise<void> {
  const server = servers.value[index]
  if (!server || busy.value) return
  await persist(servers.value.map((value, position) => ({
    ...value, enabled: position === index ? enabled : value.enabled
  })))
}

async function refresh(): Promise<void> {
  if (busy.value) return
  busy.value = true
  ++loadVersion
  try {
    const result = await request('/admin/api/mcp-config/refresh', {method: 'POST'})
    if (result.success !== true || !Array.isArray(result.status)) throw new Error('刷新工具失败')
    if (active) {
      statuses.value = result.status as Status[]
      const failed = statuses.value.some(status => !!status.error)
      toast?.(failed ? '部分服务刷新失败，请查看错误信息' : '工具列表已刷新', failed)
    }
  } catch (cause) {
    if (active) toast?.(errorMessage(cause), true)
  } finally {
    busy.value = false
  }
}

onMounted(async () => {
  await load()
  if (active) timer = setInterval(() => {
    if (!document.hidden) void load(true)
  }, 5000)
})

onUnmounted(() => {
  active = false
  if (timer) clearInterval(timer)
  for (const controller of requests) controller.abort()
  if (dialog.value) document.body.style.overflow = previousOverflow
})
</script>

<template>
  <div>
    <div class="page-header mcp-header">
      <h1 class="page-title">MCP 服务</h1>
      <div class="mcp-actions">
        <button class="btn btn-secondary" :disabled="busy || loading || !!error" @click="refresh">刷新工具</button>
        <button class="btn btn-primary" :disabled="busy || loading || !!error || servers.length >= 16" @click="open(null)">
          <NavIcon name="tool"/>添加服务
        </button>
      </div>
    </div>
    <p v-if="loading" role="status">加载中...</p>
    <div v-else-if="error" role="alert"><p>{{ error }}</p><button class="btn btn-secondary" @click="load()">重试</button></div>
    <div v-else class="mcp-list">
      <p v-if="!servers.length" class="empty-state">暂无 MCP 服务</p>
      <article v-for="(server, index) in servers" :key="server.name" class="mcp-row">
        <EnabledSwitch :model-value="server.enabled" :disabled="busy" :label="`启用服务 ${server.name}`"
                           @update:model-value="toggle(index, $event)" />
        <div class="mcp-details">
          <h2>{{ server.name }}</h2><p class="mcp-url">{{ server.url }}</p>
          <p v-if="server.description" class="mcp-description">{{ server.description }}</p>
          <p class="mcp-status" :class="{'mcp-error': server.enabled && statusByName.get(server.name)?.error}">
            {{ statusText(server) }}
          </p>
        </div>
        <div class="mcp-actions">
          <button class="btn btn-secondary btn-sm" :disabled="busy" @click="open(index)">编辑</button>
          <button class="btn btn-danger btn-sm" :disabled="busy" @click="remove(index)">删除</button>
        </div>
        <details v-if="statusByName.get(server.name)?.loaded" class="mcp-tools">
          <summary>支持的工具（{{ statusByName.get(server.name)?.toolCount }}）</summary>
          <ul v-if="statusByName.get(server.name)?.tools?.length">
            <li v-for="tool in statusByName.get(server.name)?.tools" :key="tool.name">
              <code>{{ tool.name }}</code>
              <p v-if="tool.description">{{ tool.description }}</p>
            </li>
          </ul>
          <p v-else class="mcp-status">暂无工具</p>
        </details>
      </article>
    </div>
    <div v-if="dialog" class="mcp-overlay" @click.self="!busy && (dialog = false)">
      <form ref="dialogElement" class="mcp-dialog" role="dialog" aria-modal="true" aria-labelledby="mcp-title" @submit.prevent="save"
            @keydown="handleDialogKey">
        <h2 id="mcp-title">{{ editing === null ? '添加 MCP 服务' : '编辑 MCP 服务' }}</h2>
        <div class="form-group"><label class="form-label" for="mcp-name">服务名称</label>
          <input id="mcp-name" v-model="draft.name" class="form-input" maxlength="20" pattern="[A-Za-z0-9_-]{1,20}" required :disabled="busy" autofocus></div>
        <div class="form-group"><label class="form-label" for="mcp-url">服务地址</label>
          <input id="mcp-url" v-model="draft.url" class="form-input" type="url" maxlength="4096" required :disabled="busy" placeholder="https://example.com/mcp"></div>
        <div class="form-group"><label class="form-label" for="mcp-description">用途描述</label>
          <textarea id="mcp-description" v-model="draft.description" class="form-input" rows="3" maxlength="1024" :disabled="busy"></textarea>
          <p class="form-hint" :class="{'mcp-error': descriptionBytes > 1024}">{{ descriptionBytes }} / 1024 字节</p>
        </div>
        <div class="form-group"><label class="form-label" for="mcp-token">Bearer Token</label>
          <input id="mcp-token" v-model="draft.token" class="form-input" type="password" autocomplete="new-password" :disabled="busy"></div>
        <div class="form-group">
          <div class="mcp-actions mcp-header-label"><span class="form-label">自定义请求头</span>
            <button class="btn btn-secondary btn-sm" type="button" :disabled="busy" @click="headerRows.push({name: '', value: ''})">添加请求头</button>
          </div>
          <div v-for="(header, index) in headerRows" :key="index" class="mcp-header-row">
            <input v-model="header.name" class="form-input" :aria-label="`请求头 ${index + 1} 名称`" placeholder="X-API-Key" :disabled="busy" required>
            <input v-model="header.value" class="form-input" :aria-label="`请求头 ${index + 1} 值`" type="password" autocomplete="new-password" placeholder="值" :disabled="busy">
            <button class="btn btn-danger btn-sm" type="button" :disabled="busy" :aria-label="`删除请求头 ${index + 1}`" @click="headerRows.splice(index, 1)">删除</button>
          </div>
          <p v-if="!validHeaders" class="form-hint mcp-error">请求头名称必须合法且不能重复，值不能包含换行或空字符。</p>
        </div>
        <div class="form-group"><label class="form-label" for="mcp-timeout">请求超时（秒）</label>
          <input id="mcp-timeout" v-model.number="draft.timeoutSeconds" class="form-input" type="number" min="1" max="120" required :disabled="busy"></div>
        <label class="mcp-enabled"><input v-model="draft.enabled" type="checkbox" :disabled="busy">启用服务</label>
        <div class="mcp-actions mcp-footer"><button class="btn btn-secondary" type="button" :disabled="busy" @click="dialog = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="busy || !valid">{{ busy ? '保存中...' : '保存' }}</button></div>
      </form>
    </div>
  </div>
</template>

<style scoped>
.mcp-header, .mcp-actions {display: flex; align-items: center; gap: 12px}
.mcp-header {justify-content: space-between; flex-wrap: wrap}
.mcp-actions {flex-wrap: wrap}
.mcp-actions svg {width: 16px; height: 16px}
.mcp-list {border-top: 1px solid var(--border)}
.mcp-row {display: grid; grid-template-columns: 120px minmax(0, 1fr) auto; align-items: start;
  gap: 12px 20px; padding: 20px 0; border-bottom: 1px solid var(--border)}
.mcp-row > .tool-switch {align-self: start}
.mcp-row > .mcp-actions {justify-content: flex-end}
.mcp-details {min-width: 0}
.mcp-details h2, .mcp-dialog h2 {font-size: 18px; margin: 0 0 8px}
.mcp-url, .mcp-status, .mcp-description {overflow-wrap: anywhere; margin: 6px 0; color: var(--text-secondary)}
.mcp-error {color: var(--danger)}
.mcp-tools {grid-column: 2 / -1; min-width: 0}
.mcp-tools summary {cursor: pointer; color: var(--text-secondary); padding: 8px 0}
.mcp-tools summary:focus-visible {outline: 2px solid var(--primary-strong); outline-offset: 3px}
.mcp-tools ul {list-style: none; padding: 0; margin: 0; max-height: 360px; overflow-y: auto}
.mcp-tools li {display: grid; grid-template-columns: 176px minmax(0, 1fr); gap: 20px;
  align-items: baseline; padding: 12px 0; border-top: 1px solid var(--border); overflow-wrap: anywhere}
.mcp-tools code {font-size: 13px; padding: 0; background: transparent}
.mcp-tools li p {margin: 0; color: var(--text-secondary); white-space: pre-wrap; overflow-wrap: anywhere; line-height: 1.65}
.mcp-overlay {position: fixed; inset: 0; z-index: 1000; background: rgba(0,0,0,.5); display: grid; place-items: center; padding: 20px}
.mcp-dialog {background: var(--card-bg); border: 1px solid var(--border); border-radius: 0; padding: 24px; width: min(520px, 100%); max-height: calc(100dvh - 40px); overflow-y: auto; box-sizing: border-box}
.mcp-dialog textarea.form-input {min-height: 96px; height: 112px}
.mcp-enabled {display: flex; align-items: center; gap: 8px}
.mcp-footer {justify-content: flex-end; margin-top: 24px}
.mcp-header-label {justify-content: space-between; margin-bottom: 8px}
.mcp-header-row {display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) auto; gap: 8px; margin-top: 8px; align-items: center}
@media (max-width: 800px) {
  .mcp-row {grid-template-columns: 120px minmax(0, 1fr)}
  .mcp-row > .mcp-actions {grid-column: 2; justify-content: flex-start}
  .mcp-tools {grid-column: 1 / -1}
  .mcp-tools li {grid-template-columns: minmax(0, 1fr); gap: 6px}
}
@media (max-width: 600px) {.mcp-dialog {padding: 18px}}
@media (max-width: 440px) {
  .mcp-row {grid-template-columns: minmax(0, 1fr)}
  .mcp-row > .mcp-actions {grid-column: 1}
  .mcp-header-row {grid-template-columns: minmax(0, 1fr) auto}
  .mcp-header-row input:first-child {grid-column: 1 / -1}
}
</style>
