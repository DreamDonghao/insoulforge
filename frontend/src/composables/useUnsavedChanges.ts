import {inject, onBeforeUnmount, onMounted} from 'vue'

export interface LeaveGuard {
  dirty: () => boolean
  busy: () => boolean
}

export function confirmDiscard(dirty: boolean): boolean {
  return !dirty || window.confirm('有未保存的修改，确定放弃并离开吗？')
}

export function useUnsavedChanges(dirty: () => boolean, busy: () => boolean = () => false): void {
  const guards = inject<Set<LeaveGuard> | null>('leaveGuards', null)
  const guard = {dirty, busy}
  const beforeUnload = (event: BeforeUnloadEvent): void => {
    if (!dirty() && !busy()) return
    event.preventDefault()
    event.returnValue = ''
  }
  onMounted(() => {
    guards?.add(guard)
    window.addEventListener('beforeunload', beforeUnload)
  })
  onBeforeUnmount(() => {
    guards?.delete(guard)
    window.removeEventListener('beforeunload', beforeUnload)
  })
}
