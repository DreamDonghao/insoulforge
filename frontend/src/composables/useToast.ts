import {ref, shallowRef} from 'vue'

interface Notification {
  id: number
  message: string
  error: boolean
}

const notifications = ref<Notification[]>([])
const toastTarget = shallowRef<HTMLElement | null>(null)
const targets: HTMLElement[] = []
const timers = new Map<number, ReturnType<typeof setTimeout>>()
let nextId = 0

// 原生模态框会使背景不可交互，通知必须进入当前弹窗的可访问区域。
export function registerToastTarget(target: HTMLElement): () => void {
  targets.push(target)
  toastTarget.value = target
  return () => {
    const index = targets.indexOf(target)
    if (index !== -1) targets.splice(index, 1)
    toastTarget.value = targets[targets.length - 1] ?? null
  }
}

export function useToast() {
  const dismissToast = (id: number): void => {
    clearTimeout(timers.get(id))
    timers.delete(id)
    notifications.value = notifications.value.filter(item => item.id !== id)
  }
  const showToast = (message: string, error = false): void => {
    if (notifications.value.some(item => item.message === message && item.error === error)) return
    // 成功提示只保留最新一条，但不覆盖尚未处理的错误。
    if (!error) notifications.value.filter(item => !item.error).forEach(item => dismissToast(item.id))
    const id = ++nextId
    notifications.value.push({id, message, error})
    if (!error) timers.set(id, setTimeout(() => dismissToast(id), 3000))
  }
  return {notifications, toastTarget, showToast, dismissToast}
}
