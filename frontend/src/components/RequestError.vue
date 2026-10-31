<script setup lang="ts">
withDefaults(defineProps<{message: string, busy?: boolean, stale?: boolean}>(), {busy: false, stale: false})
defineEmits<{retry: []}>()
</script>

<template>
  <div v-if="message" class="request-error">
    <p role="alert">{{ message }}<span v-if="stale"> 当前显示上次成功加载的数据。</span></p>
    <button type="button" class="btn btn-secondary btn-sm" :disabled="busy" @click="$emit('retry')">
      {{ busy ? '重试中…' : '重试' }}
    </button>
  </div>
</template>

<style scoped>
.request-error {
  margin: 12px 0;
  padding: 12px;
  border: 1px solid var(--danger);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}
p { margin: 0; color: var(--text-primary); overflow-wrap: anywhere; }
</style>
