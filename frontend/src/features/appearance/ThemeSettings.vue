<script setup lang="ts">
import {ref, watch} from 'vue'
import {ACCENT_PRESETS, DEFAULT_ACCENT, normalizeAccent} from '../../utils/theme'

const props = defineProps<{accent: string, storageError: string}>()
const emit = defineEmits<{change: [color: string]}>()
const draft = ref(props.accent)
const error = ref('')
watch(() => props.accent, color => { draft.value = color; error.value = '' })
const choose = (color: string): void => {
  draft.value = color
  error.value = ''
  emit('change', color)
}
const updateHex = (): void => {
  const color = normalizeAccent(draft.value)
  if (color) choose(color)
  else error.value = '请输入十六进制颜色，例如 #3B6FD8。'
}
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">主题配色</h1>
      <p class="page-subtitle">统一配置全局强调色</p>
    </div>
    <section class="card theme-settings" aria-labelledby="theme-settings-title">
      <h2 id="theme-settings-title" class="card-title theme-title">主题颜色</h2>
      <p class="theme-description">选择后立即应用到所有页面，自动适配浅色与深色模式。</p>
      <fieldset class="color-presets">
        <legend>预设配色</legend>
        <label v-for="preset in ACCENT_PRESETS" :key="preset.color" class="color-preset" :class="{selected: accent === preset.color}">
          <input type="radio" name="theme-accent" :checked="accent === preset.color" @change="choose(preset.color)">
          <span class="color-swatch" :style="{backgroundColor: preset.color}" aria-hidden="true"></span>
          <span>{{ preset.name }}</span>
        </label>
      </fieldset>
      <div class="custom-color">
        <label class="form-label" for="accent-hex">自定义颜色（HEX）</label>
        <div class="custom-color-inputs">
          <label for="accent-picker" class="sr-only">选取主题颜色</label>
          <input id="accent-picker" class="color-picker" type="color" :value="accent" @input="choose(($event.target as HTMLInputElement).value)">
          <input id="accent-hex" v-model="draft" class="form-input" type="text" maxlength="7" spellcheck="false" autocomplete="off"
                 :aria-invalid="!!error" :aria-describedby="error ? 'accent-error' : undefined"
                 @change="updateHex" @keydown.enter.prevent="updateHex">
        </div>
        <p v-if="error" id="accent-error" class="form-error" role="alert">{{ error }}</p>
      </div>
      <footer class="theme-footer">
        <button type="button" class="btn btn-secondary btn-sm" @click="choose(DEFAULT_ACCENT)">恢复默认</button>
        <p v-if="storageError" class="storage-error" role="alert">{{ storageError }}</p>
        <p v-else class="theme-description" role="status">仅保存在当前浏览器，换设备或清除网站数据后需重新设置。</p>
      </footer>
    </section>
    <section class="card" aria-labelledby="theme-preview-title">
      <h2 id="theme-preview-title" class="card-title">配色预览</h2>
      <div class="theme-preview" role="img" aria-label="当前主题的选中项、强调文字与主按钮预览">
        <span class="preview-selection">选中项</span>
        <span class="preview-link">强调文字</span>
        <span class="btn btn-primary preview-button">主按钮</span>
      </div>
    </section>
  </div>
</template>

<style scoped>
.theme-title { margin-bottom: 12px; }
.theme-description { margin: 0; color: var(--text-secondary); font-size: 12px; }
.color-presets { max-width: 600px; margin: 24px 0; padding: 0; border: 0; display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
.color-presets legend { margin-bottom: 10px; font-size: 13px; font-weight: 500; }
.color-preset { display: flex; align-items: center; gap: 6px; min-height: 40px; padding: 8px; border: 1px solid var(--border); border-radius: var(--radius-sm); cursor: pointer; font-size: 12px; }
.color-preset.selected { background: var(--primary-soft); border-color: var(--primary); font-weight: 600; }
.color-preset input { margin: 0; accent-color: var(--primary-strong); }
.color-swatch { width: 14px; height: 14px; flex-shrink: 0; border: 1px solid var(--border); border-radius: var(--radius-xs); }
.custom-color { max-width: 440px; }
.custom-color-inputs { display: flex; align-items: center; gap: 8px; }
.custom-color-inputs .form-input { height: 36px; padding-block: 6px; }
.color-picker { width: 44px; height: 36px; flex-shrink: 0; padding: 4px; border: 1px solid var(--control-border); border-radius: var(--radius-sm); background: var(--input-bg); cursor: pointer; }
.color-picker::-webkit-color-swatch-wrapper { padding: 0; }
.color-picker::-webkit-color-swatch { border: 0; border-radius: var(--radius-xs); }
.color-picker::-moz-color-swatch { border: 0; border-radius: var(--radius-xs); }
.theme-preview { display: flex; align-items: center; flex-wrap: wrap; gap: 12px; margin-top: 16px; }
.preview-button { cursor: default; }
.preview-selection { padding: 8px 12px; background: var(--primary-soft); color: var(--primary); border-radius: var(--radius-sm); font-weight: 600; }
.preview-link { color: var(--primary); }
.theme-footer { display: flex; align-items: center; flex-wrap: wrap; gap: 12px; margin-top: 24px; }
.storage-error { margin: 0; color: var(--danger); font-size: 12px; }
@media (max-width: 440px) { .color-presets { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
</style>
