<script setup lang="ts">
import {reactive} from 'vue'
import ModalDialog from './components/ModalDialog.vue'
import DesktopApp from './desktop/DesktopApp.vue'
import DesktopLogin from './desktop/DesktopLogin.vue'
import MobileApp from './mobile/MobileApp.vue'
import MobileLogin from './mobile/MobileLogin.vue'
import {useAdminApp} from './shared/useAdminApp'
const {systemNavItems, dailyNavItems, leaveGuards, canLeave, isMobile, mobileNavigationOpen, changeView, currentView, sidebarNav, navIndicator, updateNavIndicator, resetPageScroll, authenticated, authenticationChecked, accessToken, loginError, loginSubmitting, showAccessToken, theme, accentColor, accentStorageError, setAccentColor, applyTheme, initTheme, toggleTheme, themeToggleLabel, notifications, toastTarget, showToast, dismissToast, qqConfig, avatarUrl, avatarFailed, wsConnected, ws, reconnectTimer, disconnectWebSocket, loadQQConfig, connectWebSocket, initializeAdmin, checkAuthentication, loginWithToken, login, loginFromUrlFragment, logoutConfirmationVisible, logoutSubmitting, logoutError, logoutHasUnsavedChanges, logout, cancelLogout, confirmLogout}=useAdminApp()
const loginState=reactive({authenticationChecked,theme,themeToggleLabel,toggleTheme,login,accessToken,loginError,loginSubmitting,showAccessToken})
const desktopState=reactive({currentView,dailyNavItems,systemNavItems,avatarUrl,avatarFailed,qqConfig,navIndicator,sidebarNav,changeView,logoutSubmitting,mobileNavigationOpen,isMobile,logout,themeToggleLabel,theme,toggleTheme,resetPageScroll,accentColor,accentStorageError,setAccentColor})
</script>

<template>
  <MobileLogin v-if="isMobile && (!authenticationChecked || !authenticated)" :checked="authenticationChecked" v-model:token="accessToken" :error="loginError" :busy="loginSubmitting" :theme-label="themeToggleLabel" @submit="login" @theme="toggleTheme"/>
  <DesktopLogin v-else-if="!authenticationChecked || !authenticated" :state="loginState"/>

  <div v-else class="container">
    <MobileApp v-if="isMobile" :view="currentView" :navigate="changeView" :accent="accentColor" :storage-error="accentStorageError" @theme="toggleTheme" @logout="logout" @change="setAccentColor"/>
    <template v-else>
    <DesktopApp :state="desktopState"/>
    </template>
  </div>

  <ModalDialog v-if="authenticated && logoutConfirmationVisible" class="logout-overlay" labelledby="logout-title" @close="cancelLogout">
    <section class="logout-dialog" :aria-busy="logoutSubmitting">
      <header class="logout-header">
        <h2 id="logout-title">退出登录</h2>
        <button class="btn btn-secondary btn-sm" type="button" aria-label="关闭退出确认" :disabled="logoutSubmitting" @click="cancelLogout">×</button>
      </header>
      <p>确定退出登录吗？退出后需要重新登录。</p>
      <p v-if="logoutHasUnsavedChanges()" class="logout-warning">有未保存的修改，退出后这些修改将丢失。</p>
      <p v-if="logoutError" class="logout-error" role="alert">{{ logoutError }}</p>
      <footer class="logout-actions">
        <button class="btn btn-secondary" type="button" data-modal-initial-focus :disabled="logoutSubmitting" @click="cancelLogout">取消</button>
        <button class="btn btn-primary" type="button" :disabled="logoutSubmitting" @click="confirmLogout">{{ logoutSubmitting ? '退出中…' : '退出登录' }}</button>
      </footer>
    </section>
  </ModalDialog>

  <!-- Toast 提示 -->
  <Teleport :to="toastTarget ?? 'body'">
    <div v-if="notifications.length" class="toast-stack" aria-label="操作通知">
      <div v-for="notification in notifications" :key="notification.id" class="toast"
           :class="{error: notification.error}">
        <span :role="notification.error ? 'alert' : 'status'" aria-atomic="true">{{ notification.message }}</span>
        <button type="button" class="toast-dismiss" aria-label="关闭此提示" @click="dismissToast(notification.id)">×</button>
      </div>
    </div>
  </Teleport>
</template>

<style>
.logout-overlay { display: grid; place-items: center; padding: 20px; background: rgba(0, 0, 0, 0.5); }
.logout-dialog { width: min(420px, 100%); max-height: 100%; overflow-y: auto; padding: 24px; border: 1px solid var(--border); border-radius: var(--radius-lg); background: var(--card-bg); color: var(--text-primary); }
.logout-header { display: flex; align-items: center; justify-content: space-between; gap: 16px; margin-bottom: 16px; }
.logout-header h2 { margin: 0; font-size: 18px; }
.logout-dialog p { margin: 0 0 12px; }
.logout-warning { color: var(--warning); }
.logout-error { color: var(--danger); }
.logout-actions { display: flex; justify-content: flex-end; flex-wrap: wrap; gap: 12px; margin-top: 24px; }
</style>
