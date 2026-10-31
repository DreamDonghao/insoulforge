<script setup lang="ts">
import {nextTick, ref, watch} from 'vue'

const props = withDefaults(defineProps<{
  modelValue: string
  items: readonly {key: string, label: string}[]
  label: string
  idPrefix: string
  itemMinWidth?: number
}>(), {itemMinWidth: 104})
const emit = defineEmits<{'update:modelValue': [value: string]}>()
const tablist = ref<HTMLElement | null>(null)
const focusedKey = ref(props.modelValue)
const tabId = (key: string): string => `${props.idPrefix}-tab-${encodeURIComponent(key)}`
const panelId = (key: string): string => `${props.idPrefix}-panel-${encodeURIComponent(key)}`
watch(() => props.modelValue, value => { focusedKey.value = value })

const focusTab = (key: string): void => {
  focusedKey.value = key
  const tab = tablist.value?.querySelector<HTMLElement>(`[id="${tabId(key)}"]`)
  tab?.focus()
  tab?.scrollIntoView({block: 'nearest', inline: 'nearest'})
}
const moveFocus = (event: KeyboardEvent, key: string): void => {
  const index = props.items.findIndex(item => item.key === key)
  let target: number
  switch (event.key) {
    case 'ArrowRight': target = (index + 1) % props.items.length; break
    case 'ArrowLeft': target = (index - 1 + props.items.length) % props.items.length; break
    case 'Home': target = 0; break
    case 'End': target = props.items.length - 1; break
    default: return
  }
  event.preventDefault()
  const item = props.items[target]
  if (item) focusTab(item.key)
}
// 方向键只移动焦点，Enter／空格通过原生按钮激活，避免触发未保存修改确认。
const activate = async (key: string): Promise<void> => {
  emit('update:modelValue', key)
  await nextTick()
  // 父页面可能拒绝切换，焦点与最终选中的标签保持一致。
  focusTab(props.modelValue)
}
</script>

<template>
  <div class="sliding-tabs-root">
    <p :id="`${idPrefix}-instructions`" class="sr-only">使用左右方向键、Home 或 End 移动焦点，按 Enter 或空格切换内容。</p>
    <div class="sliding-tabs-scroll">
      <div ref="tablist" class="tabs sliding-tabs" role="tablist" :aria-label="label" :aria-describedby="`${idPrefix}-instructions`" aria-orientation="horizontal"
           :style="{'--tab-count': items.length, '--tab-index': Math.max(0, items.findIndex(item => item.key === modelValue)), minWidth: `${items.length * itemMinWidth}px`}">
        <span class="sliding-tab-indicator" aria-hidden="true"></span>
        <button v-for="item in items" :id="tabId(item.key)" :key="item.key" type="button" class="tab" role="tab"
                :class="{active: modelValue === item.key}" :aria-selected="modelValue === item.key"
                :aria-controls="panelId(item.key)" :tabindex="focusedKey === item.key ? 0 : -1"
                @focus="focusedKey = item.key" @keydown="moveFocus($event, item.key)"
                @click="activate(item.key)">{{ item.label }}</button>
      </div>
    </div>
    <section v-for="item in items" :id="panelId(item.key)" :key="item.key" role="tabpanel"
             :aria-labelledby="tabId(item.key)" :hidden="modelValue !== item.key" tabindex="0">
      <slot v-if="modelValue === item.key" name="panel" :item="item"/>
    </section>
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

.tab:focus-visible { outline: 2px solid var(--focus-ring); outline-offset: -3px; }

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
