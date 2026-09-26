<script lang="ts" setup>
/**
 * @file OneBotConfig.vue
 * @brief OneBot 配置组件
 */
import {inject, onMounted, ref, type Ref} from 'vue'
import type {ApiResponse, QQConfig} from '../../vite-env.d'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')
const qqConfig = inject<QQConfig>('qqConfig')
const saving: Ref<boolean> = ref(false)
const activeTransport: Ref<QQConfig['oneBotTransport'] | null> = ref(null)

const refreshActiveTransport = async (): Promise<void> => {
  const resp = await fetch('/admin/api/qq-config')
  const config: Partial<QQConfig> = await resp.json()
  if (config.oneBotTransport === 'http' || config.oneBotTransport === 'websocket') {
    activeTransport.value = config.oneBotTransport
  }
}

const saveQQConfig = async (): Promise<void> => {
  saving.value = true
  try {
    const resp = await fetch('/admin/api/qq-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(qqConfig)
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      activeTransport.value = qqConfig!.oneBotTransport
      showToast!('OneBot 配置已保存')
    } else {
      showToast!(data.error || '保存失败', true)
    }
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

    <div class="card">
      <section class="config-section">
        <h3 class="section-title">机器人身份</h3>
        <div class="form-row">
          <div class="form-group">
            <label class="form-label">Bot QQ 号</label>
            <input v-model.number="qqConfig!.selfQQNumber" class="form-input" type="number">
            <p class="form-hint">机器人自身的 QQ 号</p>
          </div>
          <div class="form-group">
            <label class="form-label">Bot 名称</label>
            <input v-model="qqConfig!.botName" class="form-input" type="text">
            <p class="form-hint">机器人在群聊中的显示名称</p>
          </div>
        </div>
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
            <div aria-label="OneBot 传输方式" class="transport-switch" role="radiogroup">
              <button
                  :aria-checked="qqConfig!.oneBotTransport === 'http'"
                  :class="{active: qqConfig!.oneBotTransport === 'http'}"
                  role="radio"
                  type="button"
                  @click="qqConfig!.oneBotTransport = 'http'"
              >
                HTTP
              </button>
              <button
                  :aria-checked="qqConfig!.oneBotTransport === 'websocket'"
                  :class="{active: qqConfig!.oneBotTransport === 'websocket'}"
                  role="radio"
                  type="button"
                  @click="qqConfig!.oneBotTransport = 'websocket'"
              >
                WebSocket
              </button>
            </div>
          </div>
        </div>
        <p class="form-hint transport-hint">选择新的方式并保存后，才会切换当前运行方式。</p>

        <div v-if="qqConfig!.oneBotTransport === 'http'" class="connection-fields">
          <div class="form-row">
            <div class="form-group">
              <label class="form-label">HTTP 服务地址</label>
              <input v-model="qqConfig!.qqHttpHost" class="form-input" placeholder="http://127.0.0.1:3000"
                     type="text">
              <p class="form-hint">OneBot HTTP 服务地址</p>
            </div>
            <div class="form-group">
              <label class="form-label">Access Token</label>
              <input v-model="qqConfig!.accessToken" class="form-input" type="text">
              <p class="form-hint">OneBot API 访问令牌</p>
            </div>
          </div>
        </div>

        <div v-else class="connection-fields">
          <div class="form-row">
            <div class="form-group">
              <label class="form-label">WebSocket 服务地址</label>
              <input v-model="qqConfig!.qqWebSocketHost" class="form-input" placeholder="ws://127.0.0.1:3001"
                     type="text">
              <p class="form-hint">OneBot 正向 WebSocket 地址</p>
            </div>
            <div class="form-group">
              <label class="form-label">Access Token</label>
              <input v-model="qqConfig!.accessToken" class="form-input" type="text">
              <p class="form-hint">OneBot API 访问令牌</p>
            </div>
          </div>
        </div>
      </section>

      <button :disabled="saving" class="btn btn-primary" @click="saveQQConfig">
        {{ saving ? '保存中...' : '保存配置' }}
      </button>
    </div>
  </div>
</template>

<style scoped>
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
  font-size: 15px;
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
  border-radius: 8px;
}

.transport-switch button {
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

.transport-switch button + button {
  border-left: 1px solid var(--border);
}

.transport-switch button:hover {
  color: var(--text-primary);
  background: var(--bg-secondary);
}

.transport-switch button.active {
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
