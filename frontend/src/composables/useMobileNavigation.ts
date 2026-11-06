import {onBeforeUnmount, onMounted, ref, watchEffect} from 'vue'

export function useMobileNavigation(canSwitch: () => boolean = () => true) {
  const isMobile = ref(false)
  const mobileNavigationOpen = ref(false)
  const requested = ref(false)
  watchEffect(() => { if (requested.value !== isMobile.value && canSwitch()) isMobile.value = requested.value }, {flush: 'sync'})
  let media: MediaQueryList | undefined
  const update = (): void => {
    requested.value = media?.matches ?? false
    mobileNavigationOpen.value = false
  }
  onMounted(() => {
    if (typeof window.matchMedia !== 'function') return
    media = window.matchMedia('(max-width: 760px)')
    update()
    media.addEventListener('change', update)
  })
  onBeforeUnmount(() => media?.removeEventListener('change', update))
  return {isMobile, mobileNavigationOpen}
}
