<script lang="ts">
const scrollLocks = new Set<symbol>()
let previousOverflow = ''
</script>

<script setup lang="ts">
import {onBeforeUnmount, onMounted, ref} from 'vue'
import {registerToastTarget} from '../composables/useToast'

defineProps<{labelledby: string}>()
const emit = defineEmits<{close: []}>()
const element = ref<HTMLDialogElement | null>(null)
const lock = Symbol('modal')
let previousFocus: HTMLElement | null = null
let releaseToastTarget: (() => void) | undefined

const handleKeydown = (event: KeyboardEvent): void => {
  if (event.key !== 'Tab') return
  const dialog = element.value!
  const controls = Array.from(dialog.querySelectorAll<HTMLElement>(
      'a[href], button, input, select, textarea, [tabindex], [contenteditable="true"]'))
      .filter(control => control.tabIndex >= 0 && !control.matches(':disabled') &&
          !control.closest('[hidden], [inert]') && control.getClientRects().length > 0)
  const first = controls[0]
  const last = controls[controls.length - 1]
  if (!first || !last) {
    event.preventDefault()
    dialog.focus()
  } else if (event.shiftKey && (document.activeElement === first || !controls.includes(document.activeElement as HTMLElement))) {
    event.preventDefault()
    last.focus()
  } else if (!event.shiftKey && (document.activeElement === last || !dialog.contains(document.activeElement))) {
    event.preventDefault()
    first.focus()
  }
}

onMounted(() => {
  const dialog = element.value!
  previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
  if (scrollLocks.size === 0) previousOverflow = document.body.style.overflow
  scrollLocks.add(lock)
  document.body.style.overflow = 'hidden'
  dialog.showModal()
  releaseToastTarget = registerToastTarget(dialog)
  // 编辑弹窗优先进入字段；只读弹窗可显式指定标题作为起点。
  const initial = dialog.querySelector<HTMLElement>('[data-modal-initial-focus]') ??
      dialog.querySelector<HTMLElement>('input:not(:disabled):not([type="hidden"]), select:not(:disabled), textarea:not(:disabled)')
  initial?.focus()
})

onBeforeUnmount(() => {
  releaseToastTarget?.()
  element.value?.close()
  scrollLocks.delete(lock)
  if (scrollLocks.size === 0) document.body.style.overflow = previousOverflow
  if (previousFocus?.isConnected && !previousFocus.closest('[inert]')) previousFocus.focus()
  else document.querySelector<HTMLElement>('.nav-item.active')?.focus()
})
</script>

<template>
  <dialog ref="element" :aria-labelledby="labelledby" aria-modal="true"
          @keydown="handleKeydown" @cancel.prevent="emit('close')" @click.self="emit('close')">
    <slot/>
  </dialog>
</template>

<style scoped>
dialog {
  width: 100%;
  height: 100%;
  max-width: none;
  max-height: none;
  margin: 0;
  padding: 0;
  border: 0;
  color: inherit;
}

dialog:not([open]) {
  display: none;
}

dialog::backdrop {
  background: transparent;
}
</style>
