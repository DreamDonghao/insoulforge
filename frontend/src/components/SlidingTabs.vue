<script setup lang="ts">
withDefaults(defineProps<{
  modelValue: string
  items: readonly {key: string, label: string}[]
  label: string
  itemMinWidth?: number
}>(), {itemMinWidth: 104})

const emit = defineEmits<{'update:modelValue': [value: string]}>()
</script>

<template>
  <div class="sliding-tabs-scroll">
    <div class="tabs sliding-tabs" role="group" :aria-label="label"
         :style="{'--tab-count': items.length, '--tab-index': Math.max(0, items.findIndex(item => item.key === modelValue)), minWidth: `${items.length * itemMinWidth}px`}">
      <span class="sliding-tab-indicator" aria-hidden="true"></span>
      <button v-for="item in items" :key="item.key" type="button" class="tab"
              :class="{active: modelValue === item.key}" :aria-pressed="modelValue === item.key"
              @click="emit('update:modelValue', item.key)">{{ item.label }}</button>
    </div>
  </div>
</template>

<style scoped>
.sliding-tabs-scroll {
  overflow-x: auto;
  margin-bottom: 20px;
}

.sliding-tabs {
  position: relative;
  isolation: isolate;
  display: flex;
  width: 100%;
  margin-bottom: 0;
  gap: 0;
}

.tab {
  position: relative;
  flex: 1 0 0;
  min-height: 42px;
  padding: 10px 12px;
  white-space: nowrap;
  transition: color 160ms ease;
}

.tab.active {
  background: transparent;
  border-color: transparent;
}

.sliding-tab-indicator {
  position: absolute;
  inset: 0 auto 0 0;
  width: calc(100% / var(--tab-count));
  z-index: -1;
  pointer-events: none;
  background: var(--primary-soft);
  border-bottom: 2px solid var(--primary);
  transform: translateX(calc(var(--tab-index) * 100%));
  transition: transform 220ms cubic-bezier(0.22, 0.61, 0.36, 1);
}

@media (prefers-reduced-motion: reduce) {
  .tab,
  .sliding-tab-indicator {
    transition: none;
  }
}
</style>
