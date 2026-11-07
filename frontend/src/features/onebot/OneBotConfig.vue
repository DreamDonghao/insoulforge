<script lang="ts" setup>
/**
 * @file OneBotConfig.vue
 * @brief OneBot 配置组件
 */
import NavIcon from '../../components/NavIcon.vue'
import {inject, onMounted, ref, type Ref, watch} from 'vue'
import type {ApiResponse, QQConfig} from '../../vite-env.d'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
import FieldHelp from '../../components/FieldHelp.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {identifierError} from '../../utils/validation'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const qqConfig = inject<QQConfig>('qqConfig')
const saving: Ref<boolean> = ref(false)
const showAccessToken = ref(false)
watch(() => qqConfig?.oneBotTransport, () => { showAccessToken.value = false }, {flush: 'sync'})
const activeTransport: Ref<QQConfig['oneBotTransport'] | null> = ref(null)

const loading = ref(false)
const loaded = ref(false)
const loadError = ref('')
const savedConfig = ref<string | null>(null)
const isDirty = (): boolean => savedConfig.value !== null && JSON.stringify(qqConfig!) !== savedConfig.value
useUnsavedChanges(isDirty, () => saving.value)
const refreshActiveTransport = async (): Promise<void> => {
  if (loading.value || saving.value || !confirmDiscard(isDirty())) return
  loading.value = true
  try {
    const config = await requestJson<QQConfig>('/admin/api/qq-config')
    if (config.oneBotTransport !== 'http' && config.oneBotTransport !== 'websocket' && config.oneBotTransport !== 'reverse_websocket') throw new Error('OneBot 配置数据无效')
    if (config.oneBotImplementation !== 'napcat' && config.oneBotImplementation !== 'snowluma') throw new Error('OneBot 实现类型无效')
    Object.assign(qqConfig!, config, {selfQQNumber: String(config.selfQQNumber ?? '0')})
    activeTransport.value = config.oneBotTransport
    loaded.value = true
    savedConfig.value = JSON.stringify(qqConfig!)
    loadError.value = ''
  } catch (cause) {
    loadError.value = requestError(cause, 'OneBot 配置加载失败')
  } finally {
    loading.value = false
  }
}

const {errors, fieldAttrs, validateForm} = useFormValidation(() => {
  const result: Record<string, string> = {'bot-qq': identifierError(qqConfig!.selfQQNumber, 'Bot 用户 ID', true)}
  if (qqConfig!.oneBotTransport === 'reverse_websocket') {
    if (!qqConfig!.accessToken.trim()) result['onebot-access-token'] = '反向 WebSocket 必须配置 Access Token'
    return result
  }
  const http = qqConfig!.oneBotTransport === 'http'
  const value = http ? qqConfig!.qqHttpHost : qqConfig!.qqWebSocketHost
  try {
    const url = new URL(value)
    if (!(http ? ['http:', 'https:'] : ['ws:', 'wss:']).includes(url.protocol) || !url.hostname) throw new Error()
  } catch {
    result[http ? 'onebot-http-host' : 'onebot-ws-host'] = http ? '请输入有效的 HTTP 或 HTTPS 服务地址' : '请输入有效的 WS 或 WSS 服务地址'
  }
  return result
})

const reverseAddress = 'ws://<主程序地址>:<端口>/onebot/ws'
const copyReverseAddress = async (): Promise<void> => {
  try { await navigator.clipboard.writeText(reverseAddress); showToast!('接入地址已复制') }
  catch { showToast!('复制失败，请手动复制接入地址', true) }
}

const saveQQConfig = async (): Promise<void> => {
  if (saving.value || loading.value || !loaded.value || !validateForm()) return
  const transport = qqConfig!.oneBotTransport
  const submitted = JSON.stringify({...qqConfig, selfQQNumber: qqConfig!.selfQQNumber.trim()})
  saving.value = true
  try {
    const resp = await fetch('/admin/api/qq-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: submitted
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      activeTransport.value = transport
      if (JSON.stringify({...qqConfig, selfQQNumber: qqConfig!.selfQQNumber.trim()}) === submitted) qqConfig!.selfQQNumber = qqConfig!.selfQQNumber.trim()
      savedConfig.value = submitted
      showToast!('OneBot 配置已保存')
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, 'OneBot 配置保存失败'), true)
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  void refreshActiveTransport()
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">OneBot 配置</h1>
      <p class="page-subtitle">配置 OneBot 协议连接参数</p>
    </div>

    <RequestError :message="loadError" :busy="loading" @retry="refreshActiveTransport"/>
    <p v-if="loading" role="status">加载中…</p>
    <form v-if="loaded" class="onebot-form" novalidate @submit.prevent="saveQQConfig">
      <section class="config-section">
        <h3 class="section-title">机器人身份</h3>
        <div class="form-group">
          <div class="field-label">
            <label for="bot-qq" class="form-label">Bot 用户 ID</label>
            <FieldHelp id="bot-qq-hint" label="Bot 用户 ID">机器人自身的用户 ID，0 表示尚未配置。</FieldHelp>
          </div>
          <input v-model="qqConfig!.selfQQNumber" v-bind="fieldAttrs('bot-qq', 'bot-qq-hint')" class="form-input" type="text" inputmode="numeric" autocomplete="off" :disabled="saving">
          <FieldError field="bot-qq" :message="errors['bot-qq']"/>
        </div>
        <div class="form-group">
          <div class="field-label">
            <label for="bot-name" class="form-label">Bot 名称</label>
            <FieldHelp id="bot-name-hint" label="Bot 名称">机器人在群聊中的显示名称。</FieldHelp>
          </div>
          <input id="bot-name" :disabled="saving" v-model="qqConfig!.botName" class="form-input" type="text" aria-describedby="bot-name-hint">
        </div>
      </section>

      <section class="config-section">
        <h3 class="section-title">OneBot 实现</h3>
        <div class="form-group">
          <div class="field-label">
            <span id="onebot-implementation-label" class="form-label">实现类型</span>
            <FieldHelp id="onebot-implementation-hint" label="OneBot 实现类型">保存后切换扩展接口。连接地址和 Token 不会自动更改。</FieldHelp>
          </div>
          <fieldset class="transport-switch" :disabled="saving" aria-labelledby="onebot-implementation-label" aria-describedby="onebot-implementation-note">
            <legend class="sr-only">OneBot 实现类型（保存后生效）</legend>
            <label v-for="implementation in ['napcat', 'snowluma'] as const" :key="implementation" :class="{active: qqConfig!.oneBotImplementation === implementation}">
              <input class="sr-only" v-model="qqConfig!.oneBotImplementation" type="radio" name="onebot-implementation" :value="implementation">
              <span class="choice-mark" aria-hidden="true">{{ qqConfig!.oneBotImplementation === implementation ? '✓' : '' }}</span>
              {{ implementation === 'napcat' ? 'NapCat' : 'SnowLuma' }}
            </label>
          </fieldset>
        </div>
        <p id="onebot-implementation-note" class="setting-summary">连接地址和 Token 需与所选服务匹配。</p>
      </section>

      <section class="config-section">
        <div class="connection-header">
          <h3 class="section-title">连接方式</h3>
          <p class="runtime-transport" role="status">当前运行：{{ activeTransport === null ? '读取中...' : activeTransport === 'http' ? 'HTTP' : activeTransport === 'reverse_websocket' ? '反向 WebSocket' : '正向 WebSocket' }}</p>
        </div>
        <div class="form-group">
          <div class="field-label">
            <span id="onebot-transport-label" class="form-label">传输方式</span>
            <FieldHelp id="onebot-transport-hint" label="传输方式">选择新的方式并保存后，才会切换当前运行方式。</FieldHelp>
          </div>
          <fieldset class="transport-switch transport-modes" :disabled="saving" aria-labelledby="onebot-transport-label" aria-describedby="onebot-transport-hint">
            <legend class="sr-only">OneBot 传输方式（保存后生效）</legend>
            <label v-for="transport in ['http', 'websocket', 'reverse_websocket'] as const" :key="transport" :class="{active: qqConfig!.oneBotTransport === transport}">
              <input class="sr-only" v-model="qqConfig!.oneBotTransport" type="radio" name="onebot-transport" :value="transport">
              <span class="choice-mark" aria-hidden="true">{{ qqConfig!.oneBotTransport === transport ? '✓' : '' }}</span>
              {{ transport === 'http' ? 'HTTP' : transport === 'reverse_websocket' ? '反向 WebSocket' : '正向 WebSocket' }}
            </label>
          </fieldset>
        </div>
        <p v-if="activeTransport !== null && qqConfig!.oneBotTransport !== activeTransport" class="transport-pending" role="status">保存后切换至 {{ qqConfig!.oneBotTransport === 'http' ? 'HTTP' : qqConfig!.oneBotTransport === 'reverse_websocket' ? '反向 WebSocket' : '正向 WebSocket' }}。</p>
        <div v-if="qqConfig!.oneBotTransport === 'http'" class="form-group">
          <label for="onebot-http-host" class="form-label">HTTP 服务地址</label>
          <input v-bind="fieldAttrs('onebot-http-host')" :disabled="saving" v-model="qqConfig!.qqHttpHost" class="form-input" placeholder="http://<OneBot 服务地址>:<端口>" type="text">
          <FieldError field="onebot-http-host" :message="errors['onebot-http-host']"/>
        </div>
        <div v-else-if="qqConfig!.oneBotTransport === 'websocket'" class="form-group">
          <div class="field-label">
            <label for="onebot-ws-host" class="form-label">WebSocket 服务地址</label>
            <FieldHelp id="onebot-ws-host-hint" label="WebSocket 服务地址">OneBot 正向 WebSocket 地址。</FieldHelp>
          </div>
          <input v-bind="fieldAttrs('onebot-ws-host', 'onebot-ws-host-hint')" :disabled="saving" v-model="qqConfig!.qqWebSocketHost" class="form-input" placeholder="ws://<OneBot 服务地址>:<端口>" type="text">
          <FieldError field="onebot-ws-host" :message="errors['onebot-ws-host']"/>
        </div>
        <p v-if="qqConfig!.oneBotTransport === 'reverse_websocket'"><code>{{ reverseAddress }}</code><button type="button" class="btn btn-secondary" @click="copyReverseAddress">复制地址</button></p>
        <div class="form-group">
          <div class="field-label">
            <label for="onebot-access-token" class="form-label">Access Token</label>
            <FieldHelp id="onebot-access-token-hint" label="Access Token">OneBot API 访问令牌。</FieldHelp>
          </div>
          <div class="secret-input">
            <input v-bind="fieldAttrs('onebot-access-token', 'onebot-access-token-hint')" :disabled="saving" v-model="qqConfig!.accessToken" class="form-input"
                   :type="showAccessToken ? 'text' : 'password'" autocomplete="off" spellcheck="false" aria-describedby="onebot-access-token-hint">
            <button class="secret-toggle" type="button"
                    aria-controls="onebot-access-token" :title="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'"
                    :aria-label="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'" :aria-pressed="showAccessToken" @click="showAccessToken = !showAccessToken">
              <NavIcon :name="showAccessToken ? 'eye-off' : 'eye'"/>
            </button>
          </div>
          <FieldError field="onebot-access-token" :message="errors['onebot-access-token']"/>
        </div>
      </section>

      <div class="save-actions">
        <button :disabled="saving" class="btn btn-primary" type="submit">{{ saving ? '保存中...' : '保存配置' }}</button>
        <p class="save-hint" :class="{'has-changes': isDirty()}" role="status">{{ isDirty() ? '有未保存的修改' : '修改后需保存' }}</p>
      </div>
    </form>
  </div>
</template>

<style scoped>
.onebot-form { display: grid; gap: 16px; }
.config-section { min-width: 0; padding: 20px 24px; background: var(--card-bg); border-radius: var(--radius-md); }
.section-title { margin: 0 0 12px; color: var(--text-secondary); font-size: 16px; font-weight: 600; }
.form-group {
  position: relative;
  display: grid;
  grid-template-columns: minmax(0, 220px) minmax(0, 440px) minmax(0, 1fr);
  align-items: center;
  gap: 8px 16px;
  min-width: 0;
  margin: 0;
  padding: 12px 0;
}
.form-group:first-of-type { padding-top: 0; }
.connection-header + .form-group { padding-top: 0; }
.form-group:last-child { padding-bottom: 0; }
.form-group + .form-group::before {
  content: '';
  position: absolute;
  top: 0;
  inset-inline-start: 0;
  width: min(676px, 100%);
  border-top: 1px solid color-mix(in srgb, var(--border) 35%, transparent);
  pointer-events: none;
}
.field-label { display: flex; align-items: center; gap: 8px; min-width: 0; }
.form-label { margin: 0; }
.form-input { height: 36px; min-height: 36px; padding: 6px 10px; line-height: 22px; background-color: color-mix(in srgb, var(--input-bg) 75%, var(--card-bg)); }
.form-group :deep(.form-error) { grid-column: 2 / -1; margin: 0; }
.connection-header { display: flex; align-items: center; flex-wrap: wrap; justify-content: space-between; gap: 8px 16px; margin-bottom: 12px; }
.connection-header .section-title { margin: 0; }
.runtime-transport { margin: 0; color: var(--text-secondary); font-size: 13px; }
.transport-switch {
  justify-self: start;
  margin: 0;
  padding: 0;
  min-inline-size: 0;
  display: inline-grid;
  grid-template-columns: repeat(2, 128px);
  overflow: hidden;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
}
.transport-modes { grid-template-columns: repeat(3, minmax(0, 1fr)); width: 100%; }
.transport-switch label {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  min-height: 34px;
  padding: 6px 12px;
  color: var(--text-secondary);
  background: var(--input-bg);
  cursor: pointer;
  font-family: inherit;
  font-size: 13px;
  font-weight: 500;
  transition: background-color 0.15s ease, color 0.15s ease;
}
.transport-modes label { gap: 6px; padding-inline: 8px; white-space: nowrap; }
.transport-switch label:focus-within { outline: 2px solid var(--focus-ring); outline-offset: -2px; }
.transport-switch label + label { border-left: 1px solid var(--border); }
.transport-switch label:hover { color: var(--text-primary); background: var(--bg-secondary); }
.transport-switch label.active { color: var(--primary); background: var(--primary-soft); font-weight: 600; }
.setting-summary, .transport-pending, .save-hint { margin: 0; color: var(--text-secondary); font-size: 12px; }
.transport-pending { margin-bottom: 12px; color: var(--warning); }
.save-actions { display: flex; align-items: center; flex-wrap: wrap; gap: 8px 12px; }
.save-hint.has-changes { color: var(--warning); font-weight: 500; }
@media (max-width: 600px) {
  .form-group { grid-template-columns: minmax(0, 1fr); }
  .form-group :deep(.form-error) { grid-column: 1 / -1; }
  .transport-switch { grid-template-columns: repeat(2, minmax(0, 1fr)); width: 100%; }
  .transport-modes { grid-template-columns: minmax(0, 1fr); }
  .transport-modes label + label { border-left: 0; border-top: 1px solid var(--border); }
}
</style>
