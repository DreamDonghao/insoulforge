<script lang="ts" setup>
/**
 * @file CustomTools.vue
 * @brief 自定义工具管理组件
 */
import {computed, inject, nextTick, onMounted, reactive, ref, type Ref} from 'vue'
import {marked} from 'marked'
import type {ApiResponse} from '../../vite-env'
import SlidingTabs from '../../components/SlidingTabs.vue'
import EnabledSwitch from '../../components/EnabledSwitch.vue'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

interface CustomTool {
  id: number
  name: string
  description: string
  parameters: string
  executorType: 'lua'
  scriptContent: string
  readme: string
  enabled: boolean
}

const tools: Ref<CustomTool[]> = ref([])
interface BuiltinTool {
  name: string
  category: string
  enabled: boolean
  description: string
  defaultDescription: string
  customDescription: boolean
}
const activeTab = ref('builtin')
const toolTabs = [{key: 'builtin', label: '内置工具'}, {key: 'custom', label: '自定义工具'}]
const builtinTools = ref<BuiltinTool[]>([])
const builtinLoading = ref(true)
const builtinError = ref('')
const builtinFilter = ref('')
const customFilter = ref('')
const pendingTools = reactive(new Set<string>())
const visibleCustomTools = computed(() => tools.value.filter(tool =>
  `${tool.name} ${tool.description}`.toLowerCase().includes(customFilter.value.toLowerCase())))
const builtinSaving = ref(false)
const editingBuiltin = ref<BuiltinTool | null>(null)
const visibleBuiltins = computed(() => builtinTools.value.filter(tool =>
  `${tool.name} ${tool.description}`.toLowerCase().includes(builtinFilter.value.toLowerCase())))
const loadBuiltins = async (): Promise<void> => {
  builtinLoading.value = true
  builtinError.value = ''
  try {
    const resp = await fetch('/admin/api/builtin-tools')
    if (!resp.ok) throw new Error('内置工具加载失败')
    builtinTools.value = await resp.json()
  } catch (error) {
    builtinError.value = error instanceof Error ? error.message : '内置工具加载失败'
  } finally {
    builtinLoading.value = false
  }
}
const persistBuiltin = async (name: string, patch: {enabled?: boolean, description?: string | null}): Promise<void> => {
  const resp = await fetch('/admin/api/builtin-tools', {
    method: 'PUT', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({name, ...patch})
  })
  const result: ApiResponse = await resp.json()
  if (!resp.ok || !result.success) throw new Error(result.error || '保存失败')
}
const toggleBuiltin = async (tool: BuiltinTool, enabled: boolean): Promise<void> => {
  const key = `builtin:${tool.name}`
  if (pendingTools.has(key)) return
  pendingTools.add(key)
  try {
    await persistBuiltin(tool.name, {enabled})
    tool.enabled = enabled
    showToast?.(enabled ? '工具已启用' : '工具已禁用')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    pendingTools.delete(key)
  }
}
const saveBuiltin = async (): Promise<void> => {
  const tool = editingBuiltin.value
  if (!tool || builtinSaving.value) return
  builtinSaving.value = true
  try {
    const description = tool.customDescription ? tool.description : null
    await persistBuiltin(tool.name, {description})
    const existing = builtinTools.value.find(item => item.name === tool.name)
    if (existing) {
      existing.description = description ?? tool.defaultDescription
      existing.customDescription = description !== null
    }
    editingBuiltin.value = null
    showToast?.('内置工具配置已保存')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '保存失败', true)
  } finally {
    builtinSaving.value = false
  }
}
const loading: Ref<boolean> = ref(false)
const saving: Ref<boolean> = ref(false)
const showDialog: Ref<boolean> = ref(false)
const isEdit: Ref<boolean> = ref(false)
const testing: Ref<boolean> = ref(false)
const testResult: Ref<string> = ref('')

// 导入对话框
const showImportDialog: Ref<boolean> = ref(false)
const importJson: Ref<string> = ref('')
const importing: Ref<boolean> = ref(false)

// README 详情对话框
const showReadmeDialog: Ref<boolean> = ref(false)
const currentReadme: Ref<string> = ref('')
const currentToolName: Ref<string> = ref('')

const defaultLuaScript = `function run(args, ctx)
    return "会话 " .. ctx.session_id .. "：" .. (args.text or "你好")
end

-- 可选：由 bot.start_task(description, payload) 启动后台入口。
-- function background(payload, ctx)
--     return "任务完成"
-- end`

const editTool = reactive<CustomTool>({
  id: 0,
  name: '',
  description: '',
  parameters: '{"type":"object","properties":{},"required":[]}',
  executorType: 'lua',
  scriptContent: defaultLuaScript,
  readme: '',
  enabled: true
})

const testArgs: Ref<string> = ref('{}')

const loadTools = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch('/admin/api/custom-tools')
    tools.value = await resp.json()
  } finally {
    loading.value = false
  }
}

const autoResize = (event: Event): void => {
  const target = event.target as HTMLTextAreaElement
  target.style.height = 'auto'
  target.style.height = target.scrollHeight + 'px'
}

const resizeAllTextareas = (): void => {
  nextTick(() => {
    document.querySelectorAll('.auto-height').forEach((el) => {
      const textarea = el as HTMLTextAreaElement
      textarea.style.height = 'auto'
      textarea.style.height = textarea.scrollHeight + 'px'
    })
  })
}

const openAddDialog = (): void => {
  isEdit.value = false
  Object.assign(editTool, {
    id: 0,
    name: '',
    description: '',
    parameters: '{"type":"object","properties":{},"required":[]}',
    executorType: 'lua',
    scriptContent: defaultLuaScript,
    readme: '',
    enabled: true
  })
  testResult.value = ''
  testArgs.value = '{}'
  showDialog.value = true
  resizeAllTextareas()
}

const openEditDialog = (tool: CustomTool): void => {
  isEdit.value = true
  Object.assign(editTool, tool)
  testResult.value = ''
  testArgs.value = '{}'
  showDialog.value = true
  resizeAllTextareas()
}

const closeDialog = (): void => {
  showDialog.value = false
}

const saveTool = async (): Promise<void> => {
  if (!editTool.name) {
    showToast!('请填写工具名称', true)
    return
  }
  if (!editTool.description) {
    showToast!('请填写工具描述', true)
    return
  }
  if (!editTool.scriptContent) {
    showToast!('请填写脚本内容', true)
    return
  }

  try {
    JSON.parse(editTool.parameters)
  } catch {
    showToast!('参数定义 JSON 格式错误', true)
    return
  }

  saving.value = true
  try {
    const url = isEdit.value
        ? `/admin/api/custom-tool/${editTool.id}`
        : '/admin/api/custom-tool'
    const method = isEdit.value ? 'PUT' : 'POST'

    const resp = await fetch(url, {
      method,
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(editTool)
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!(isEdit.value ? '工具已更新' : '工具已添加')
      closeDialog()
      loadTools()
    } else {
      showToast!(data.error || '操作失败', true)
    }
  } finally {
    saving.value = false
  }
}

const deleteTool = async (id: number): Promise<void> => {
  if (!confirm('确定删除此工具？')) return

  const resp = await fetch(`/admin/api/custom-tool/${id}`, {method: 'DELETE'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('工具已删除')
    loadTools()
  }
}

const toggleTool = async (tool: CustomTool): Promise<void> => {
  const key = `custom:${tool.id}`
  if (pendingTools.has(key)) return
  pendingTools.add(key)
  try {
    const resp = await fetch(`/admin/api/custom-tool/${tool.id}/toggle`, {method: 'POST'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '状态保存失败')
    tool.enabled = !tool.enabled
    showToast?.(tool.enabled ? '工具已启用' : '工具已禁用')
  } catch (error) {
    showToast?.(error instanceof Error ? error.message : '状态保存失败', true)
  } finally {
    pendingTools.delete(key)
  }
}

const reloadTools = async (): Promise<void> => {
  const resp = await fetch('/admin/api/custom-tools/reload', {method: 'POST'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('工具已重新加载')
  }
}

const exportTool = async (id: number, name: string): Promise<void> => {
  const resp = await fetch(`/admin/api/custom-tool/${id}/export`)
  const data = await resp.json()
  if (data.success === false) {
    showToast!(data.error || '导出失败', true)
    return
  }
  // 创建下载文件
  const blob = new Blob([JSON.stringify(data, null, 2)], {type: 'application/json'})
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `${name}.json`
  a.click()
  URL.revokeObjectURL(url)
}

const openImportDialog = (): void => {
  importJson.value = ''
  showImportDialog.value = true
}

const handleFileUpload = (event: Event): void => {
  const target = event.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return

  const reader = new FileReader()
  reader.onload = (e) => {
    importJson.value = e.target?.result as string || ''
  }
  reader.readAsText(file)
  target.value = '' // 清空，允许重复选择同一文件
}

const importTool = async (): Promise<void> => {
  if (!importJson.value.trim()) {
    showToast!('请输入或上传工具 JSON', true)
    return
  }

  try {
    const toolData = JSON.parse(importJson.value)
    importing.value = true
    const resp = await fetch('/admin/api/custom-tool/import', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(toolData)
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('工具已导入')
      showImportDialog.value = false
      loadTools()
    } else {
      showToast!(data.error || '导入失败', true)
    }
  } catch {
    showToast!('JSON 格式错误', true)
  } finally {
    importing.value = false
  }
}

const showReadme = (tool: CustomTool): void => {
  currentToolName.value = tool.name
  if (tool.readme) {
    currentReadme.value = marked(tool.readme) as string
  } else {
    currentReadme.value = '<p style="color: var(--text-light);">暂无说明文档</p>'
  }
  showReadmeDialog.value = true
}

const testCurrentTool = async (): Promise<void> => {
  testing.value = true
  testResult.value = ''
  try {
    let testArgsJson
    try {
      testArgsJson = JSON.parse(testArgs.value)
    } catch {
      showToast!('测试参数 JSON 格式错误', true)
      return
    }

    const resp = await fetch('/admin/api/custom-tool/test', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        executorType: editTool.executorType,
        scriptContent: editTool.scriptContent,
        args: testArgsJson
      })
    })
    const data = await resp.json()
    if (data.success) {
      testResult.value = data.result
    } else {
      testResult.value = '错误: ' + (data.error || '测试失败')
    }
  } finally {
    testing.value = false
  }
}


onMounted(() => {
  loadTools()
  loadBuiltins()
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">工具配置</h1>
      <p class="page-subtitle">管理内置工具与自定义工具</p>
    </div>

    <SlidingTabs v-model="activeTab" :items="toolTabs" label="工具类型" />
    <section v-if="activeTab === 'builtin'" class="builtin-tools">
      <div class="tools-toolbar">
        <input v-model="builtinFilter" class="form-input tools-search" type="search" placeholder="搜索名称或描述"
               aria-label="搜索内置工具">
        <span class="tools-count">{{ builtinTools.filter(tool => tool.enabled).length }} / {{ builtinTools.length }} 已启用</span>
      </div>
      <p v-if="builtinLoading" class="empty-state">加载中...</p>
      <div v-else-if="builtinError" class="empty-state">
        <p>{{ builtinError }}</p><button class="btn btn-secondary" @click="loadBuiltins">重试</button>
      </div>
      <template v-else>
        <section v-for="category in ['回复', '信息', '动作']" :key="category" class="builtin-group">
          <h3>{{ category }}工具 <span>{{ visibleBuiltins.filter(tool => tool.category === category).length }}</span></h3>
          <div v-for="tool in visibleBuiltins.filter(item => item.category === category)" :key="tool.name"
               class="builtin-row">
            <EnabledSwitch :model-value="tool.enabled" :disabled="pendingTools.has(`builtin:${tool.name}`)"
                               :label="`启用工具 ${tool.name}`" @update:model-value="toggleBuiltin(tool, $event)" />
            <div class="tool-identity"><code>{{ tool.name }}</code>
              <span v-if="tool.customDescription" class="tool-kind">已修改描述</span>
            </div>
            <p class="tool-description">{{ tool.description || '—' }}</p>
            <div class="tool-controls">
              <button class="btn btn-secondary btn-sm" :disabled="pendingTools.has(`builtin:${tool.name}`)"
                      @click="editingBuiltin = {...tool}">编辑描述</button>
            </div>
          </div>
        </section>
        <p v-if="!visibleBuiltins.length" class="empty-state">暂无匹配的内置工具</p>
      </template>
    </section>
    <section v-if="activeTab === 'custom'" class="custom-tools">
      <div class="tools-toolbar">
        <input v-model="customFilter" class="form-input tools-search" type="search" placeholder="搜索名称或描述"
               aria-label="搜索自定义工具">
        <span class="tools-count">{{ tools.filter(tool => tool.enabled).length }} / {{ tools.length }} 已启用</span>
        <div class="tool-actions">
          <button class="btn btn-secondary btn-sm" @click="openImportDialog">导入</button>
          <button class="btn btn-secondary btn-sm" @click="reloadTools">重新加载</button>
          <button class="btn btn-primary btn-sm" @click="openAddDialog">添加工具</button>
        </div>
      </div>
      <p v-if="loading" class="empty-state">加载中...</p>
      <section v-else class="builtin-group">
        <h3>Lua 工具 <span>{{ visibleCustomTools.length }}</span></h3>
        <div v-for="tool in visibleCustomTools" :key="tool.id" class="builtin-row">
          <EnabledSwitch :model-value="tool.enabled" :disabled="pendingTools.has(`custom:${tool.id}`)"
                             :label="`启用工具 ${tool.name}`" @update:model-value="toggleTool(tool)" />
          <div class="tool-identity"><code>{{ tool.name }}</code><span class="tool-kind">Lua</span></div>
          <p class="tool-description">{{ tool.description || '—' }}</p>
          <div class="tool-controls">
            <div class="tool-actions">
              <button class="btn btn-secondary btn-sm" @click="showReadme(tool)">说明</button>
              <button class="btn btn-secondary btn-sm" @click="exportTool(tool.id, tool.name)">导出</button>
              <button class="btn btn-secondary btn-sm" :disabled="pendingTools.has(`custom:${tool.id}`)"
                      @click="openEditDialog(tool)">编辑</button>
              <button class="btn btn-danger btn-sm" :disabled="pendingTools.has(`custom:${tool.id}`)"
                      @click="deleteTool(tool.id)">删除</button>
            </div>
          </div>
        </div>
        <p v-if="!visibleCustomTools.length" class="empty-state">{{ tools.length ? '暂无匹配的工具' : '暂无自定义工具' }}</p>
      </section>
    </section>

    <div v-if="editingBuiltin" class="dialog-overlay">
      <form class="dialog builtin-description-dialog" role="dialog" aria-modal="true"
            aria-labelledby="builtin-dialog-title" @submit.prevent="saveBuiltin">
        <div class="dialog-header">
          <h3 id="builtin-dialog-title">{{ editingBuiltin.name }}</h3>
          <button type="button" class="dialog-close" :disabled="builtinSaving" @click="editingBuiltin = null">×</button>
        </div>
        <div class="dialog-body">
          <label class="form-label" for="builtin-description">工具描述</label>
          <textarea id="builtin-description" v-model="editingBuiltin.description" class="form-input"
                    rows="7" @input="editingBuiltin.customDescription = true"></textarea>
        </div>
        <div class="dialog-footer">
          <button type="button" class="btn btn-secondary" :disabled="builtinSaving"
                  @click="editingBuiltin.description = editingBuiltin.defaultDescription; editingBuiltin.customDescription = false">恢复默认描述</button>
          <button type="button" class="btn btn-secondary" :disabled="builtinSaving" @click="editingBuiltin = null">取消</button>
          <button type="submit" class="btn btn-primary" :disabled="builtinSaving">{{ builtinSaving ? '保存中...' : '保存' }}</button>
        </div>
      </form>
    </div>
    <!-- 导入对话框 -->
    <div v-if="showImportDialog" class="dialog-overlay">
      <div class="dialog" style="width: 600px;">
        <div class="dialog-header">
          <h3>导入工具</h3>
          <button class="dialog-close" @click="showImportDialog = false">×</button>
        </div>
        <div class="dialog-body">
          <div class="form-group">
            <label class="form-label">上传 JSON 文件</label>
            <input accept=".json" class="form-input" type="file" @change="handleFileUpload">
          </div>
          <div class="form-group">
            <label class="form-label">或粘贴 JSON 内容</label>
            <textarea v-model="importJson" class="form-input code-input" placeholder="粘贴工具 JSON 内容..."
                      spellcheck="false" style="min-height: 200px;"></textarea>
          </div>
          <div class="import-example">
            <strong>JSON 格式示例：</strong>
            <pre>{
  "name": "random",
  "description": "生成随机数",
  "parameters": {"type": "object", "properties": {...}},
  "executorType": "lua",
  "scriptContent": "function run(args, ctx)\n    return '完成'\nend"
}</pre>
          </div>
        </div>
        <div class="dialog-footer">
          <button class="btn btn-secondary" @click="showImportDialog = false">取消</button>
          <button :disabled="importing" class="btn btn-success" @click="importTool">
            {{ importing ? '导入中...' : '导入' }}
          </button>
        </div>
      </div>
    </div>

    <!-- 添加/编辑对话框 -->
    <div v-if="showDialog" class="dialog-overlay">
      <div style="width: 900px" class="dialog">
        <div class="dialog-header">
          <h3>{{ isEdit ? '编辑工具' : '添加工具' }}</h3>
          <button class="dialog-close" @click="closeDialog">×</button>
        </div>

        <div class="dialog-body">
          <!-- 基本信息 -->
          <div class="form-row">
            <div class="form-group">
              <label class="form-label">工具名称 *</label>
              <input v-model="editTool.name" :disabled="isEdit" class="form-input"
                     placeholder="如: search_web, calculate">
            </div>
          </div>

          <div class="form-group">
            <label class="form-label">工具描述 *</label>
            <input v-model="editTool.description" class="form-input" placeholder="描述工具功能，供 LLM 理解何时调用">
          </div>

          <div class="form-group">
            <label class="form-label">参数定义 (JSON Schema)</label>
            <textarea v-model="editTool.parameters" class="form-input code-input auto-height"
                      @input="autoResize"></textarea>
          </div>

          <div class="form-group">
            <label class="form-label">Lua 脚本</label>
            <textarea v-model="editTool.scriptContent" class="form-input code-input auto-height" spellcheck="false"
                      @input="autoResize"></textarea>
            <small class="form-hint">定义 run(args, ctx)；bot.send_message 可等待发送结果，bot.start_task 启动可选的 background 入口。测试不会真正发送。</small>
          </div>

          <!-- README 文档 -->
          <div class="form-group">
            <label class="form-label">说明文档 (Markdown)</label>
            <textarea v-model="editTool.readme" class="form-input auto-height" placeholder="作者、用法、联系方式等信息..."
                      style="min-height: 80px;" @input="autoResize"></textarea>
            <small class="form-hint">可选填写作者、使用示例、联系方式等，便于分享和查阅</small>
          </div>

          <!-- 测试区域 -->
          <div class="test-section">
            <div class="test-header">
              <span class="test-title">测试工具</span>
              <button :disabled="testing" class="btn btn-primary btn-sm" @click="testCurrentTool">
                {{ testing ? '执行中...' : '执行测试' }}
              </button>
            </div>
            <div class="test-content">
              <div class="test-input">
                <label>输入参数 (JSON)</label>
                <textarea v-model="testArgs" class="form-input code-input auto-height" @input="autoResize"></textarea>
              </div>
              <div class="test-output">
                <label>输出结果</label>
                <pre class="result-box">{{ testResult || '点击执行测试查看结果...' }}</pre>
              </div>
            </div>
          </div>

          <div class="form-group checkbox-group">
            <EnabledSwitch v-model="editTool.enabled" :disabled="saving" label="启用此工具" />
          </div>
        </div>

        <div class="dialog-footer">
          <button class="btn btn-secondary" @click="closeDialog">取消</button>
          <button :disabled="saving" class="btn btn-success" @click="saveTool">
            {{ saving ? '保存中...' : '保存' }}
          </button>
        </div>
      </div>
    </div>

    <!-- README 详情对话框 -->
    <div v-if="showReadmeDialog" class="dialog-overlay">
      <div class="dialog" style="width: 700px; max-height: 80vh;">
        <div class="dialog-header">
          <h3>{{ currentToolName }}</h3>
          <button class="dialog-close" @click="showReadmeDialog = false">×</button>
        </div>
        <div class="dialog-body readme-content" v-html="currentReadme"></div>
        <div class="dialog-footer">
          <button class="btn btn-secondary" @click="showReadmeDialog = false">关闭</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.tools-toolbar {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 12px 20px;
}
.tools-search {
  flex: 1 1 240px;
  width: auto;
  max-width: 420px;
  min-width: 0;
}
.tools-count {
  color: var(--text-secondary);
  font-size: 13px;
  white-space: nowrap;
}
.tools-toolbar > .tool-actions {
  margin-left: auto;
}
.tool-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.builtin-group {
  margin-top: 20px;
}
.builtin-group h3 {
  font-size: 16px;
  margin: 0;
  padding: 12px 0;
  border-bottom: 1px solid var(--border);
}
.builtin-group h3 span {
  margin-left: 8px;
  color: var(--text-light);
  font-size: 13px;
  font-weight: 400;
}
.builtin-row {
  display: grid;
  grid-template-columns: 120px 176px minmax(0, 1fr) var(--tool-actions-width, 96px);
  gap: 20px;
  align-items: center;
  padding: 14px 0;
  min-height: 76px;
  border-bottom: 1px solid var(--border);
}
.custom-tools {
  --tool-actions-width: 224px;
}
.builtin-row:hover {
  background: var(--row-hover);
}
.tool-identity {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 4px;
  min-width: 0;
}
.tool-identity code {
  max-width: 100%;
  overflow-wrap: anywhere;
  color: var(--text-primary);
  font-size: 14px;
  line-height: 1.6;
  padding: 0;
  background: transparent;
}
.tool-kind {
  color: var(--text-light);
  font-size: 12px;
}
.tool-description {
  margin: 0;
  min-width: 0;
  font-size: 14px;
  line-height: 1.65;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  color: var(--text-secondary);
}
.tool-controls {
  display: flex;
  justify-content: flex-end;
  justify-self: end;
}
.tool-controls > .tool-actions,
.tool-controls > .btn {
  justify-self: end;
}
.tool-controls > .tool-actions {
  justify-content: flex-end;
  gap: 6px;
}
.tool-controls .btn {
  min-height: 34px;
}
.builtin-description-dialog {
  width: 640px;
  max-width: calc(100vw - 32px);
}
.builtin-description-dialog textarea {
  display: block;
  min-height: 160px;
  height: 220px;
  max-height: 45vh;
  font-family: inherit;
  font-size: 14px;
  line-height: 1.7;
}
.builtin-description-dialog .dialog-footer {
  flex-wrap: wrap;
}
.builtin-description-dialog .dialog-footer > :first-child {
  margin-right: auto;
}
@media (max-width: 1100px) {
  .builtin-row {
    grid-template-columns: 120px minmax(0, 1fr) var(--tool-actions-width, 96px);
    gap: 8px 16px;
  }
  .tool-description {
    grid-column: 2;
    grid-row: 2;
  }
  .builtin-row > .tool-switch {
    grid-column: 1;
    grid-row: 1 / span 2;
  }
  .tool-controls {
    grid-column: 3;
    grid-row: 1 / span 2;
  }
}
@media (max-width: 800px) {
  .builtin-row {
    grid-template-columns: 120px minmax(0, 1fr);
    gap: 10px 16px;
  }
  .tool-description {
    grid-column: 1 / -1;
    grid-row: 2;
  }
  .builtin-row > .tool-switch {
    grid-column: 1;
    grid-row: 1;
  }
  .tool-controls {
    grid-column: 1 / -1;
    grid-row: 3;
  }
  .tools-toolbar > .tool-actions {
    width: 100%;
    margin-left: 0;
  }
}

.code-input {
  font-family: 'Consolas', 'Monaco', 'Menlo', monospace;
  font-size: 13px;
  line-height: 1.5;
}

.auto-height {
  min-height: 60px;
  resize: none;
  overflow: hidden;
}

.dialog-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.dialog {
  background: var(--card-bg);
  border-radius: 0;
  max-width: 95vw;
  max-height: 90vh;
  overflow-y: auto;
  box-shadow: none;
  border: 1px solid var(--border);
}

.dialog-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 14px 20px;
  border-bottom: 1px solid var(--border);
  position: sticky;
  top: 0;
  background: var(--card-bg);
  z-index: 1;
}

.dialog-header h3 {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
}

.dialog-close {
  background: none;
  border: none;
  font-size: 22px;
  cursor: pointer;
  color: var(--text-light);
  line-height: 1;
}

.dialog-close:hover {
  color: var(--text-primary);
}

.dialog-body {
  padding: 16px 20px;
}

.readme-content {
  max-height: 60vh;
  overflow-y: auto;
  line-height: 1.7;
}

.readme-content h1, .readme-content h2, .readme-content h3 {
  margin-top: 0;
  margin-bottom: 12px;
  color: var(--text-primary);
}

.readme-content h1 {
  font-size: 20px;
}

.readme-content h2 {
  font-size: 18px;
}

.readme-content h3 {
  font-size: 16px;
}

.readme-content p {
  margin: 0 0 12px 0;
}

.readme-content ul, .readme-content ol {
  margin: 0 0 12px 0;
  padding-left: 24px;
}

.readme-content code {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 2px 6px;
  border-radius: 0;
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 13px;
}

.readme-content pre {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 12px 16px;
  border-radius: 0;
  overflow-x: auto;
  margin: 0 0 12px 0;
}

.readme-content pre code {
  background: none;
  padding: 0;
  color: inherit;
}

.readme-content a {
  color: var(--primary);
  text-decoration: none;
}

.readme-content a:hover {
  text-decoration: underline;
}

.readme-content blockquote {
  border-left: 4px solid var(--border-strong);
  margin: 0 0 12px 0;
  padding-left: 16px;
  color: var(--text-secondary);
}

.form-row {
  display: flex;
  gap: 16px;
}

.form-row .form-group {
  flex: 1;
}

.form-group {
  margin-bottom: 14px;
}

.form-group:last-child {
  margin-bottom: 0;
}

.form-label {
  display: block;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-primary);
  margin-bottom: 4px;
}

.form-hint {
  color: var(--text-light);
  font-size: 12px;
  margin-top: 4px;
  display: block;
}

.checkbox-group {
  margin-top: 8px;
}

.import-example {
  margin-top: 12px;
  padding: 12px;
  background: var(--bg-secondary);
  border-radius: 0;
  font-size: 12px;
}

.import-example pre {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 8px 12px;
  border-radius: 0;
  font-size: 11px;
  margin: 8px 0;
  overflow-x: auto;
}

.test-section {
  margin-top: 16px;
  border: 1px solid var(--border);
  border-radius: 0;
  overflow: hidden;
}

.test-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 12px;
  background: var(--bg-secondary);
  border-bottom: 1px solid var(--border);
}

.test-title {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.test-content {
  padding: 12px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}

.test-input label,
.test-output label {
  display: block;
  font-size: 12px;
  color: var(--text-secondary);
  margin-bottom: 4px;
}

.result-box {
  margin: 0;
  padding: 8px 10px;
  background: var(--code-bg);
  color: var(--code-text);
  border-radius: 0;
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 12px;
  min-height: 80px;
  max-height: 200px;
  overflow-y: auto;
  white-space: pre-wrap;
  word-break: break-all;
}

.dialog-footer {
  padding: 12px 20px;
  border-top: 1px solid var(--border);
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  background: var(--bg-secondary);
  position: sticky;
  bottom: 0;
}
</style>
