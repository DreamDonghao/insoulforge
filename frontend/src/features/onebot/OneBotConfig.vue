<script lang="ts" setup>
/**
 * @file OneBotConfig.vue
 * @brief OneBot 配置组件
 */
import {inject, onMounted, ref, type Ref, watch} from 'vue'
import type {ApiResponse, QQConfig} from '../../vite-env.d'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import FieldError from '../../components/FieldError.vue'
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
    if (config.oneBotTransport !== 'http' && config.oneBotTransport !== 'websocket') throw new Error('OneBot 配置数据无效')
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
  const result: Record<string, string> = {'bot-qq': identifierError(qqConfig!.selfQQNumber, 'Bot QQ 号', true)}
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
    <form v-if="loaded" class="card" novalidate @submit.prevent="saveQQConfig">
      <section class="config-section">
        <h3 class="section-title">机器人身份</h3>
        <div class="form-row">
          <div class="form-group">
            <label for="bot-qq" class="form-label">Bot QQ 号</label>
            <input v-model="qqConfig!.selfQQNumber" v-bind="fieldAttrs('bot-qq', 'bot-qq-hint')" class="form-input" type="text" inputmode="numeric" autocomplete="off" :disabled="saving">
            <FieldError field="bot-qq" :message="errors['bot-qq']"/>
            <p id="bot-qq-hint" class="form-hint">机器人自身的 QQ 号，0 表示尚未配置。</p>
          </div>
          <div class="form-group">
            <label for="bot-name" class="form-label">Bot 名称</label>
            <input id="bot-name" :disabled="saving" v-model="qqConfig!.botName" class="form-input" type="text">
            <p class="form-hint">机器人在群聊中的显示名称</p>
          </div>
        </div>
      </section>

      <section class="config-section">
        <h3 class="section-title">新私聊默认启用</h3>
        <label class="form-checkbox"><input v-model="qqConfig!.defaultPrivateEnabled" type="checkbox" :disabled="saving">新私聊默认启用（保存后生效）</label>
      </section>

      <section class="config-section connection-section">
        <div class="connection-header">
          <div>
            <h3 class="section-title">连接方式</h3>
            <p class="runtime-transport">
              当前运行：{{ activeTransport === null ? '读取中...' : activeTransport === 'http' ? 'HTTP' : 'WebSocket' }}
            </p>
          </div>
          <div class="transport-choice">
            <fieldset class="transport-switch" :disabled="saving">
              <legend class="sr-only">OneBot 传输方式（保存后生效）</legend>
              <label v-for="transport in ['http', 'websocket'] as const" :key="transport" :class="{active: qqConfig!.oneBotTransport === transport}">
                <input class="sr-only" v-model="qqConfig!.oneBotTransport" type="radio" name="onebot-transport" :value="transport">
                <span class="choice-mark" aria-hidden="true">{{ qqConfig!.oneBotTransport === transport ? '✓' : '' }}</span>
                {{ transport === 'http' ? 'HTTP' : 'WebSocket' }}
              </label>
            </fieldset>
          </div>
        </div>
        <p class="form-hint transport-hint">选择新的方式并保存后，才会切换当前运行方式。</p>

        <div v-if="qqConfig!.oneBotTransport === 'http'" class="connection-fields">
          <div class="form-row">
            <div class="form-group">
              <label for="onebot-http-host" class="form-label">HTTP 服务地址</label>
              <input v-bind="fieldAttrs('onebot-http-host')" :disabled="saving" v-model="qqConfig!.qqHttpHost" class="form-input" placeholder="http://127.0.0.1:3000"
                     type="text">
              <FieldError field="onebot-http-host" :message="errors['onebot-http-host']"/>
              <p class="form-hint">OneBot HTTP 服务地址</p>
            </div>
            <div class="form-group">
              <label for="onebot-access-token" class="form-label">Access Token</label>
              <div class="access-token-row">
                <input id="onebot-access-token" :disabled="saving" v-model="qqConfig!.accessToken" class="form-input"
                       :type="showAccessToken ? 'text' : 'password'" autocomplete="off" spellcheck="false"
                       aria-describedby="onebot-access-token-hint">
                <button class="btn btn-secondary btn-sm access-token-toggle" type="button"
                        :aria-label="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'"
                        :aria-pressed="showAccessToken" @click="showAccessToken = !showAccessToken">
                  {{ showAccessToken ? '隐藏' : '显示' }}
                </button>
              </div>
              <p id="onebot-access-token-hint" class="form-hint">OneBot API 访问令牌</p>
            </div>
          </div>
        </div>

        <div v-else class="connection-fields">
          <div class="form-row">
            <div class="form-group">
              <label for="onebot-ws-host" class="form-label">WebSocket 服务地址</label>
              <input v-bind="fieldAttrs('onebot-ws-host')" :disabled="saving" v-model="qqConfig!.qqWebSocketHost" class="form-input" placeholder="ws://127.0.0.1:3001"
                     type="text">
              <FieldError field="onebot-ws-host" :message="errors['onebot-ws-host']"/>
              <p class="form-hint">OneBot 正向 WebSocket 地址</p>
            </div>
            <div class="form-group">
              <label for="onebot-access-token" class="form-label">Access Token</label>
              <div class="access-token-row">
                <input id="onebot-access-token" :disabled="saving" v-model="qqConfig!.accessToken" class="form-input"
                       :type="showAccessToken ? 'text' : 'password'" autocomplete="off" spellcheck="false"
                       aria-describedby="onebot-access-token-hint">
                <button class="btn btn-secondary btn-sm access-token-toggle" type="button"
                        :aria-label="showAccessToken ? '隐藏访问令牌' : '显示访问令牌'"
                        :aria-pressed="showAccessToken" @click="showAccessToken = !showAccessToken">
                  {{ showAccessToken ? '隐藏' : '显示' }}
                </button>
              </div>
              <p id="onebot-access-token-hint" class="form-hint">OneBot API 访问令牌</p>
            </div>
          </div>
        </div>
      </section>

      <button :disabled="saving" class="btn btn-primary" type="submit">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
.access-token-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.access-token-row .form-input {
  flex: 1;
  min-width: 0;
}

.access-token-toggle {
  align-self: stretch;
  flex-shrink: 0;
  min-width: 64px;
}

.config-section {
  padding: 22px 0 6px;
  border-top: 1px solid var(--border-color);
}

.config-section:first-of-type {
  padding-top: 0;
  border-top: 0;
}

.section-title {
  margin: 0 0 14px;
  color: var(--text-primary);
  font-size: 16px;
  font-weight: 600;
}

.connection-section {
  padding-bottom: 0;
}

.connection-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 20px;
}

.connection-header .section-title {
  margin-bottom: 4px;
}

.runtime-transport {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
}

.transport-choice {
  display: flex;
  align-items: center;
  gap: 14px;
}

.transport-switch {
  display: inline-grid;
  grid-template-columns: repeat(2, 128px);
  overflow: hidden;
  border: 1px solid var(--border);
  border-radius: 0;
}

.transport-switch label {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  min-height: 40px;
  padding: 8px 12px;
  color: var(--text-secondary);
  background: var(--input-bg);
  border: 0;
  cursor: pointer;
  font-family: inherit;
  font-size: 13px;
  font-weight: 500;
  transition: background-color 0.15s ease, color 0.15s ease;
}

.transport-switch label:focus-within { outline: 2px solid var(--focus-ring); outline-offset: -2px; }

.transport-switch label + label {
  border-left: 1px solid var(--border);
}

.transport-switch label:hover {
  color: var(--text-primary);
  background: var(--bg-secondary);
}

.transport-switch label.active {
  color: var(--primary);
  background: var(--primary-soft);
  font-weight: 600;
}

.transport-hint {
  margin: 10px 0 0;
}

.connection-fields {
  margin-top: 20px;
}

@media (max-width: 640px) {
  .connection-header {
    align-items: stretch;
    flex-direction: column;
    gap: 14px;
  }

  .transport-choice {
    width: 100%;
  }

  .transport-switch {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    width: 100%;
  }
}
</style>
