import {computed, nextTick, ref} from 'vue'

export function useFormValidation(validate: () => Record<string, string>) {
  const submitted = ref(false)
  const errors = computed(() => submitted.value ? validate() : {})
  const fieldAttrs = (id: string, hint?: string) => ({
    id,
    'aria-invalid': !!errors.value[id],
    'aria-describedby': [hint, errors.value[id] ? `${id}-error` : undefined].filter(Boolean).join(' ') || undefined
  })
  const validateForm = (): boolean => {
    submitted.value = true
    const first = Object.entries(validate()).find(([, message]) => !!message)
    if (!first) return true
    void nextTick(() => document.getElementById?.(first[0])?.focus())
    return false
  }
  return {errors, fieldAttrs, validateForm, resetValidation: () => { submitted.value = false }}
}
