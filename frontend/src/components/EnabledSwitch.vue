<script setup lang="ts">
const props = withDefaults(defineProps<{
  modelValue: boolean
  disabled?: boolean
  label: string
  showStatus?: boolean
}>(), {showStatus: true})
const emit = defineEmits<{'update:modelValue': [value: boolean]}>()
const onChange = (event: Event): void => {
  const input = event.target as HTMLInputElement
  emit('update:modelValue', input.checked)
  // 异步保存时保持已确认状态，父组件更新 modelValue 后再切换。
  input.checked = props.modelValue
}
</script>

<template>
  <label class="tool-switch" :class="{'is-saving': disabled}">
    <span class="switch-control">
      <input type="checkbox" role="switch" :checked="modelValue" :disabled="disabled"
             :aria-label="label" :aria-busy="disabled"
             @change="onChange">
      <span class="switch-track" aria-hidden="true"></span>
    </span>
    <span v-if="showStatus" class="switch-label" :class="{'is-enabled': modelValue}">{{ modelValue ? '已启用' : '已禁用' }}</span>
  </label>
</template>

<style scoped>
.tool-switch {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  min-height: 40px;
  cursor: pointer;
  white-space: nowrap;
}
.switch-control {
  position: relative;
  display: inline-flex;
  align-items: center;
  width: 44px;
  height: 36px;
  flex-shrink: 0;
}
input {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  margin: 0;
  opacity: 0;
  cursor: inherit;
}
.switch-track {
  width: 44px;
  height: 24px;
  background: var(--border-strong);
  pointer-events: none;
  transition: background-color 150ms ease;
}
.switch-track::before {
  content: '';
  display: block;
  width: 18px;
  height: 18px;
  margin: 3px;
  background: #fff;
  transition: transform 150ms ease;
}
input:checked + .switch-track { background: var(--primary-strong); }
input:checked + .switch-track::before { transform: translateX(20px); }
input:focus-visible + .switch-track { outline: 2px solid var(--primary); outline-offset: 3px; }
.switch-label { color: var(--text-secondary); font-size: 13px; }
.switch-label.is-enabled { color: var(--success); }
.is-saving { opacity: 0.55; cursor: wait; }
@media (prefers-reduced-motion: reduce) {
  .switch-track, .switch-track::before { transition: none; }
}
</style>
