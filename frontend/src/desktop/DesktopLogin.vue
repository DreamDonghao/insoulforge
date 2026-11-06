<script setup lang="ts">
import {toRefs} from 'vue'
interface Context {authenticationChecked:boolean;theme:'light'|'dark';themeToggleLabel:string;toggleTheme:()=>void;login:()=>unknown;accessToken:string;loginError:string;loginSubmitting:boolean;showAccessToken:boolean}
const props=defineProps<{state:Context}>()
const {authenticationChecked,theme,themeToggleLabel,toggleTheme,login,accessToken,loginError,loginSubmitting,showAccessToken}=toRefs(props.state)
</script>
<template>
  <main class="login-page">
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
      <div v-if="!authenticationChecked" aria-live="polite" class="login-panel login-status">
        <span aria-hidden="true" class="loading-indicator"></span>
        正在验证登录状态...
      </div>
      <form v-else class="login-panel" @submit.prevent="login">
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
</template>
<style scoped>
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
  border-radius: var(--radius-sm);
  background: var(--card-bg);
  color: var(--text-secondary);
  cursor: pointer;
  font: inherit;
  font-size: 13px;
  font-weight: 600;
  box-shadow: none;
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
  border-radius: var(--radius-md);
  background: var(--card-bg);
  color: var(--primary);
  box-shadow: none;
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
  border-radius: var(--radius-lg);
  background: var(--card-bg);
  box-shadow: none;
}

.login-eyebrow {
  margin-bottom: 6px;
  color: var(--primary);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0;
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
  border: 1px solid var(--control-border);
  border-radius: var(--radius-sm);
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
  border-radius: var(--radius-sm);
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
  box-shadow: none;
}

.login-error {
  margin-top: 12px;
  padding: 8px 10px;
  border-radius: var(--radius-sm);
  background: var(--danger-soft);
  color: var(--danger);
  font-size: 13px;
}

.login-button {
  border-radius: var(--radius-sm);
  border: 0;
  cursor: pointer;
  font: inherit;
  width: 100%;
  height: 42px;
  margin-top: 22px;
  background: var(--primary-strong);
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
