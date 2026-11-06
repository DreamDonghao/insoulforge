<script setup lang="ts">
import {computed, ref, onMounted, watch} from 'vue'
import {requestJson, requestError} from '../../utils/api'
import RequestError from '../../components/RequestError.vue'
import FieldHelp from '../../components/FieldHelp.vue'
interface Usage {warning?: string; total: number; combinedTotal: number; defaultTools: {count: number; tokens: number}; parts: {id: string; name: string; description: string; tokens: number}[]}
const type = ref('group')
const id = ref('')
const busy = ref(false)
const error = ref('')
const data = ref<Usage | null>(null)
const label = ref('')
const colors: Record<string, string> = {project: 'var(--chart-project)', persona: 'var(--chart-persona)', structure: 'var(--chart-structure)', services: 'var(--chart-services)', stickers: 'var(--chart-stickers)', 'group-setting': 'var(--chart-group-setting)', tools: 'var(--chart-tools)'}
const parts = computed(() => data.value ? [...data.value.parts, {
  id: 'tools', name: '默认工具定义', tokens: data.value.defaultTools.tokens,
  description: `${data.value.defaultTools.count} 个已启用的内置工具和自定义工具，包含名称、描述及参数结构，不含 MCP 工具及 MCP 管理工具。`
}] : [])
const largest = computed(() => parts.value.reduce<(typeof parts.value)[number] | undefined>((a, b) => !a || b.tokens > a.tokens ? b : a, undefined))
const percent = (tokens: number): number => data.value?.combinedTotal ? tokens / data.value.combinedTotal * 100 : 0
async function load(): Promise<void> {
  if (busy.value) return
  busy.value = true
  error.value = ''
  data.value = null
  const queryType = type.value
  const queryId = id.value.trim()
  try {
    const next = await requestJson<Usage>(`/admin/api/system-tokens?type=${queryType}&id=${encodeURIComponent(queryId)}`)
    data.value = next
    label.value = queryType === 'private' ? '私聊 · 公共 System' : queryId ? `群聊 · ${queryId}（含群聊附加设定）` : '群聊 · 公共 System（不含群聊附加设定）'
  } catch (e) { error.value = requestError(e, '读取 System Token 占用失败') }
  finally { busy.value = false }
}
onMounted(load)
watch(type, () => { id.value = ''; void load() })
</script>

<template>
  <div class="system-tokens">
    <div class="page-header"><div class="title-row"><h2 class="page-title">上下文占用</h2><FieldHelp id="context-usage-details" label="上下文占用">查看 Executor 的 System 与默认工具定义预估 Token 分布。</FieldHelp></div></div>
    <form class="card controls" @submit.prevent="load">
      <div class="session-type"><span id="session-type-label">会话类型</span>
        <div class="type-switch" role="group" aria-labelledby="session-type-label">
          <button type="button" :aria-pressed="type === 'group'" :disabled="busy" @click="type = 'group'">群聊</button>
          <button type="button" :aria-pressed="type === 'private'" :disabled="busy" @click="type = 'private'">私聊</button>
        </div>
      </div>
      <label v-if="type === 'group'">查看特定群（可选）<input v-model="id" class="form-input" :disabled="busy" placeholder="留空查看公共部分；输入群号计入群聊附加设定"></label>
      <button class="btn btn-primary" :disabled="busy">{{ busy ? '计算中…' : '查看 / 刷新' }}</button>
    </form>
    <RequestError :message="error" :busy="busy" :stale="false" @retry="load"/>
    <template v-if="data">
      <p v-if="data.warning" class="note" role="status">{{ data.warning }}</p>
      <div class="summary">
        <div class="card"><span class="muted">System + 默认工具定义预估合计</span><div class="total">{{ data.combinedTotal.toLocaleString() }} <small>Token</small></div><span class="muted">{{ label }}</span></div>
        <div class="card"><span class="muted">文本占用最多的部分</span><h3>{{ largest?.name }}</h3><span class="muted">{{ largest?.tokens.toLocaleString() }} Token · {{ percent(largest?.tokens || 0).toFixed(1) }}%</span></div>
      </div>
      <div class="card distribution">
        <h3>System 与默认工具定义组成</h3>
        <div class="stack" role="img" aria-label="System 各部分和默认工具定义的 Token 占比分布"><div v-for="part in parts" :key="part.id" :style="{width: percent(part.tokens) + '%', background: colors[part.id]}" :title="`${part.name}：${part.tokens} Token`"/></div>
        <div class="parts-grid">
        <div v-for="part in parts" :key="part.id" class="part">
          <div class="part-name"><span><i :style="{background: colors[part.id]}"/>{{ part.name }}</span><FieldHelp :id="`context-part-${part.id}-details`" :label="part.name">{{ part.description }}</FieldHelp></div>
          <div class="part-usage">
            <div class="track"><div :style="{width: percent(part.tokens) + '%', background: colors[part.id]}"/></div>
            <span class="part-percent">{{ percent(part.tokens).toFixed(1) }}%</span>
            <span class="part-tokens">{{ part.tokens.toLocaleString() }} <small>Token</small></span>
          </div>
        </div>
        </div>
      </div>
    </template>
    <div v-else-if="!busy && !error" class="card muted">正在准备 System 用量数据。</div>
    <div class="note estimation-note"><span>预估值，非实际计费用量。</span><FieldHelp id="context-estimation-details" label="Token 估算口径">使用 Qwen2 词表估算。合计为各部分估算之和，与整体分词可能略有差异；默认工具定义按完整 JSON 估算。总计不包含 MCP、User、工具调用历史和服务端聊天模板，也不是实际计费用量。</FieldHelp></div>
  </div>
</template>

<style scoped>
.system-tokens {min-width:0}
.session-type {display:flex;align-items:center;gap:10px;color:var(--text-secondary);font-size:12px}
.type-switch {display:flex;padding:3px;gap:3px;border:1px solid var(--border);border-radius:var(--radius-sm)}
.type-switch button {border:0;border-radius:var(--radius-xs);background:transparent;color:var(--text-secondary);padding:7px 18px;font:inherit;cursor:pointer}
.type-switch button[aria-pressed="true"] {background:var(--primary-soft);color:var(--primary);font-weight:600}
.type-switch button:focus-visible {outline:2px solid var(--focus-ring);outline-offset:2px}
.type-switch button:disabled {opacity:.6;cursor:wait}

.page-header {margin-bottom:12px}
.title-row, .part-name, .estimation-note {display:flex;align-items:center;gap:8px}
.title-row, .estimation-note {position:relative}
.title-row .page-title {margin:0}
.part-name {min-width:0}
.card {padding:16px}
.controls {display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:16px;padding:16px}
.controls label {display:flex;gap:10px;align-items:center;font-size:12px;color:var(--text-secondary)}
.controls label {flex:1;min-width:260px}
.controls input {flex:1;min-width:0}
.controls .form-input {padding:7px 10px}
.summary {display:grid;grid-template-columns:1.2fr 1fr;gap:24px;margin-bottom:0}
.summary .card {display:grid;gap:4px;align-content:center}
.summary h3 {font-size:16px;margin:3px 0}
.total {font-size:28px;font-weight:700;line-height:1.2;margin:2px 0;font-variant-numeric:tabular-nums}
small,.muted,.note {color:var(--text-secondary);font-size:12px}
.distribution {display:flex;flex-direction:column}
.distribution h3 {margin:0 0 12px;font-size:14px}
.stack {display:flex;height:12px;border-radius:var(--radius-xs);overflow:hidden;background:var(--border);margin-bottom:16px}
.parts-grid {display:grid;grid-template-columns:minmax(0,1fr);gap:12px}
.part {position:relative;min-width:0;padding:16px;background:var(--bg-secondary);display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.4fr);align-items:center;gap:16px 24px;border-radius:var(--radius-sm);font-size:14px}
.part-name i {display:inline-block;width:8px;height:8px;border-radius:var(--radius-xs);margin-right:7px}
.part-usage {display:grid;grid-template-columns:minmax(0,1fr) 64px 128px;align-items:center;gap:16px;min-width:0}
.part-percent,.part-tokens {text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}
.part-percent,.part-tokens small {color:var(--text-secondary);font-size:14px}
.track {height:8px;background:var(--border);border-radius:var(--radius-pill);overflow:hidden;margin-top:0}
.track div {height:100%;border-radius:var(--radius-pill)}
.note {line-height:1.6;margin-top:10px;font-size:11px}
@media(max-width:760px) {
  .session-type {gap:8px}

  .controls {align-items:end}
  .controls label {display:grid;gap:5px}
  .controls label {min-width:180px}
  .summary {gap:12px}
  .total {font-size:26px}
  .part {grid-template-columns:minmax(0,1fr);gap:12px}
  .part-usage {grid-template-columns:minmax(0,1fr) 56px 112px;gap:12px}
}
@media(max-width:420px) {.summary {grid-template-columns:1fr}.controls button {width:100%}}
</style>
