import {onBeforeUnmount, ref} from 'vue'
import type {LeaveGuard} from './useUnsavedChanges'

export function useLogoutConfirmation(guards: Set<LeaveGuard>, onLoggedOut: () => void, onBusy: () => void) {
  const visible = ref(false)
  const submitting = ref(false)
  const error = ref('')
  let controller: AbortController | null = null
  let disposed = false
  const hasUnsavedChanges = (): boolean => [...guards].some(guard => guard.dirty())
  const isBusy = (): boolean => [...guards].some(guard => guard.busy())
  const request = (): void => {
    if (submitting.value) return
    if (isBusy()) { onBusy(); return }
    error.value = ''
    visible.value = true
  }
  const cancel = (): void => {
    if (!submitting.value) visible.value = false
  }
  const confirm = async (): Promise<void> => {
    if (!visible.value || submitting.value) return
    if (isBusy()) { onBusy(); return }
    submitting.value = true
    error.value = ''
    controller = new AbortController()
    const timeout = setTimeout(() => controller?.abort(), 10000)
    try {
      const response = await fetch('/admin/api/auth/logout', {method: 'POST', signal: controller.signal})
      if (!response.ok) throw new Error('退出登录失败，请重试。')
      const data = await response.json()
      if (data.success !== true) throw new Error('退出登录失败，请重试。')
      if (disposed) return
      visible.value = false
      onLoggedOut()
    } catch {
      if (!disposed) error.value = '退出登录未完成，请检查连接后重试。'
    } finally {
      clearTimeout(timeout)
      controller = null
      submitting.value = false
    }
  }
  onBeforeUnmount(() => { disposed = true; controller?.abort() })
  return {visible, submitting, error, hasUnsavedChanges, request, cancel, confirm}
}
