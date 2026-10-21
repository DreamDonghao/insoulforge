import {onScopeDispose, ref} from 'vue'

export function useCoalescedRefresh(request: (signal: AbortSignal) => Promise<void>) {
  const loading = ref(false)
  let running: Promise<void> | undefined
  let controller: AbortController | undefined
  let pending = false
  let disposed = false

  async function drain(): Promise<void> {
    try {
      while (pending && !disposed) {
        pending = false
        controller = new AbortController()
        await request(controller.signal)
      }
    } finally {
      running = undefined
      if (!disposed) loading.value = false
    }
  }

  function refresh(): Promise<void> {
    if (disposed) return Promise.resolve()
    // Any number of notifications during a request needs only one follow-up.
    pending = true
    if (!running) {
      loading.value = true
      running = drain()
    }
    return running
  }

  onScopeDispose(() => {
    disposed = true
    pending = false
    controller?.abort()
  })

  return {refresh, loading}
}
