<script setup lang="ts">
import {ref} from 'vue'
import NavIcon from './NavIcon.vue'

defineProps<{id: string, label: string}>()
const expanded = ref(false)
const dismissed = ref(false)
const toggle = (): void => {
  expanded.value = !expanded.value
  dismissed.value = !expanded.value
}
const dismiss = (): void => {
  expanded.value = false
  dismissed.value = true
}
</script>

<template>
  <span class="field-help" :class="{expanded, dismissed}"
        @mouseenter="dismissed = false" @focusin="dismissed = false" @focusout="expanded = false"
        @keydown.esc.stop.prevent="dismiss">
    <button type="button" class="help-trigger" :aria-label="`查看${label}的详细说明`" :aria-describedby="id" @click="toggle">
      <NavIcon name="info" aria-hidden="true"/>
    </button>
    <span :id="id" class="help-details" role="tooltip"><slot/></span>
  </span>
</template>

<style scoped src="./fieldHelp.css"/>

<style scoped>
.field-help { display: inline-flex; flex-shrink: 0; }
.field-help:not(.dismissed):hover .help-details,
.field-help:not(.dismissed):focus-within .help-details,
.field-help.expanded:not(.dismissed) .help-details { visibility: visible; transition-delay: 0s; }
</style>
