import {inject, ref, watch} from 'vue'
import type {Ref} from 'vue'

interface ConnectionHooks {
  onConnected?: (socket: WebSocket) => void
  onDisconnected?: (socket: WebSocket) => void
}

export function useAdminWebSocket(
  onMessage: (event: MessageEvent) => void,
  hooks: ConnectionHooks = {}
) {
  const connection = inject<{get: () => WebSocket | null}>('ws')
  const connected = inject<Ref<boolean>>('wsConnected', ref(false))
  let socket: WebSocket | null = null

  // Vue stops this watcher on unmount; cleanup always uses the socket it bound.
  watch(connected, (ready, _, onCleanup) => {
    const current = ready ? connection?.get() : null
    if (!current || current.readyState !== WebSocket.OPEN) return
    socket = current
    current.addEventListener('message', onMessage)
    onCleanup(() => {
      current.removeEventListener('message', onMessage)
      socket = null
      hooks.onDisconnected?.(current)
    })
    hooks.onConnected?.(current)
  }, {immediate: true, flush: 'sync'})

  return {
    send(message: unknown): void {
      if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message))
    }
  }
}
