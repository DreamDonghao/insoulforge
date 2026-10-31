<script lang="ts" setup>
/**
 * @file CustomTools.vue
 * @brief 自定义工具管理组件
 */
import {computed, inject, nextTick, onMounted, reactive, ref, type Ref} from 'vue'
import {confirmDiscard, useUnsavedChanges} from '../../composables/useUnsavedChanges'
import {marked} from 'marked'
import type {ApiResponse} from '../../vite-env'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import SlidingTabs from '../../components/SlidingTabs.vue'
import ModalDialog from '../../components/ModalDialog.vue'
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
const visibleCustomTools = computed(() => {
  const query = customFilter.value.toLowerCase()
  return tools.value.filter(tool => `${tool.name} ${tool.description}`.toLowerCase().includes(query))
})
const builtinSaving = ref(false)
const editingBuiltin = ref<BuiltinTool | null>(null)
const visibleBuiltins = computed(() => {
  const query = builtinFilter.value.toLowerCase()
  return builtinTools.value.filter(tool => `${tool.name} ${tool.description}`.toLowerCase().includes(query))
})
const builtinGroups = computed(() => {
  const groups = new Map<string, BuiltinTool[]>([['回复', []], ['信息', []], ['动作', []]])
  for (const tool of visibleBuiltins.value) groups.get(tool.category)?.push(tool)
  return Array.from(groups, ([category, tools]) => ({category, tools}))
})
const enabledBuiltinCount = computed(() => builtinTools.value.filter(tool => tool.enabled).length)
const enabledCustomCount = computed(() => tools.value.filter(tool => tool.enabled).length)
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
const showEditor: Ref<boolean> = ref(false)
const editorForm = ref<HTMLFormElement | null>(null)
let editorTrigger: HTMLElement | null = null
let editorTriggerId = ''
let listScrollY = 0
const beginEditing = (): void => {
  editorTrigger = document.activeElement as HTMLElement | null
  editorTriggerId = editorTrigger?.id ?? ''
  listScrollY = window.scrollY ?? 0
  activeTab.value = 'custom'
  showEditor.value = true
  void nextTick(() => {
    window.scrollTo?.({top: 0, behavior: 'instant'})
    document.getElementById?.('tool-editor-title')?.focus()
  })
}
const finishEditing = (): void => {
  showEditor.value = false
  void nextTick(() => {
    const target = editorTrigger?.isConnected ? editorTrigger : document.getElementById?.(editorTriggerId)
    if (target) target.focus({preventScroll: true})
    else document.getElementById?.('tools-page-title')?.focus({preventScroll: true})
    window.scrollTo?.({top: listScrollY, behavior: 'instant'})
  })
}
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

const createDefaultTool = (): CustomTool => ({
  id: 0,
  name: '',
  description: '',
  parameters: '{"type":"object","properties":{},"required":[]}',
  executorType: 'lua',
  scriptContent: defaultLuaScript,
  readme: '',
  enabled: true
})
const editTool = reactive<CustomTool>(createDefaultTool())
const savedTool = ref('')
const builtinDirty = (): boolean => {
  const tool = editingBuiltin.value
  const original = builtinTools.value.find(item => item.name === tool?.name)
  return !!tool && !!original && (tool.description !== original.description || tool.customDescription !== original.customDescription)
}
const closeBuiltin = (): void => {
  if (!builtinSaving.value && confirmDiscard(builtinDirty())) editingBuiltin.value = null
}
const closeImport = (): void => {
  if (!importing.value && confirmDiscard(importJson.value.trim() !== '')) showImportDialog.value = false
}
useUnsavedChanges(() => (showEditor.value && JSON.stringify(editTool) !== savedTool.value) ||
    builtinDirty() || (showImportDialog.value && importJson.value.trim() !== ''),
    () => saving.value || testing.value || builtinSaving.value || importing.value)


const {errors, fieldAttrs, validateForm, resetValidation} = useFormValidation(() => {
  const result: Record<string, string> = {
    'tool-name': editTool.name.trim() ? '' : '请填写工具名称',
    'tool-description': editTool.description.trim() ? '' : '请填写工具描述',
    'tool-script': editTool.scriptContent.trim() ? '' : '请填写 Lua 脚本'
  }
  try {
    const schema: unknown = JSON.parse(editTool.parameters)
    if (!schema || typeof schema !== 'object' || Array.isArray(schema)) throw new Error()
  } catch { result['tool-parameters'] = '参数定义必须是有效的 JSON 对象' }
  return result
})

const importForm = useFormValidation(() => {
  try {
    const value: unknown = JSON.parse(importJson.value)
    if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error()
    return {'tool-import-json': ''}
  } catch { return {'tool-import-json': '请上传或粘贴有效的工具 JSON 对象'} }
})

const testArgs: Ref<string> = ref('{}')
const testForm = useFormValidation(() => {
  try {
    JSON.parse(testArgs.value)
    return {'tool-test-args': ''}
  } catch { return {'tool-test-args': '测试参数必须是有效的 JSON'} }
})

const toolsError = ref('')
const toolsLoaded = ref(false)
const loadTools = async (): Promise<void> => {
  if (loading.value) return
  loading.value = true
  try {
    const data = await requestJson<CustomTool[]>('/admin/api/custom-tools')
    if (!Array.isArray(data)) throw new Error('工具列表数据无效')
    tools.value = data
    toolsLoaded.value = true
    toolsError.value = ''
  } catch (cause) {
    toolsError.value = requestError(cause, '自定义工具加载失败')
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
    editorForm.value?.querySelectorAll('.auto-height').forEach((el) => {
      const textarea = el as HTMLTextAreaElement
      textarea.style.height = 'auto'
      textarea.style.height = textarea.scrollHeight + 'px'
    })
  })
}

const openAddEditor = (): void => {
  isEdit.value = false
  Object.assign(editTool, createDefaultTool())
  testResult.value = ''
  testArgs.value = '{}'
  testForm.resetValidation()
  resetValidation()
  savedTool.value = JSON.stringify(editTool)
  beginEditing()
  resizeAllTextareas()
}

const openEditEditor = (tool: CustomTool): void => {
  isEdit.value = true
  Object.assign(editTool, tool)
  testResult.value = ''
  testArgs.value = '{}'
  testForm.resetValidation()
  resetValidation()
  savedTool.value = JSON.stringify(editTool)
  beginEditing()
  resizeAllTextareas()
}

const closeEditor = (): void => {
  if (saving.value || testing.value) {
    showToast?.('保存或测试执行中，请稍后返回', true)
    return
  }
  if (confirmDiscard(JSON.stringify(editTool) !== savedTool.value)) finishEditing()
}

const saveTool = async (): Promise<void> => {
  if (saving.value || testing.value || !validateForm()) return
  const snapshot = JSON.stringify(editTool)
  saving.value = true
  try {
    const url = isEdit.value
        ? `/admin/api/custom-tool/${editTool.id}`
        : '/admin/api/custom-tool'
    const method = isEdit.value ? 'PUT' : 'POST'

    const resp = await fetch(url, {
      method,
      headers: {'Content-Type': 'application/json'},
      body: snapshot
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!(isEdit.value ? '工具已更新' : '工具已添加')
      savedTool.value = snapshot
      if (JSON.stringify(editTool) === snapshot) finishEditing()
      loadTools()
    } else {
      showToast!(data.error || '操作失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '保存工具失败'), true)
  } finally {
    saving.value = false
  }
}

const deleteTool = async (id: number): Promise<void> => {
  const key = `custom:${id}`
  if (pendingTools.has(key) || !window.confirm('确定删除此工具？')) return
  pendingTools.add(key)
  try {

    const resp = await fetch(`/admin/api/custom-tool/${id}`, {method: 'DELETE'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      showToast!('工具已删除')
      loadTools()
    }
  } catch (cause) {
    showToast!(requestError(cause, '删除工具失败'), true)
  } finally {
    pendingTools.delete(key)
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
  try {
    const resp = await fetch('/admin/api/custom-tools/reload', {method: 'POST'})
    const data: ApiResponse = await resp.json()
    if (!resp.ok || !data.success) throw new Error(data.error || '操作未完成')
    if (data.success) {
      showToast!('工具已重新加载')
    }
  } catch (cause) {
    showToast!(requestError(cause, '重新加载工具失败'), true)
  }
}

const exportTool = async (id: number, name: string): Promise<void> => {
  try {
    const resp = await fetch(`/admin/api/custom-tool/${id}/export`)
    const data = await resp.json()
    if (!resp.ok) throw new Error(data.error || '操作未完成')
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
  } catch (cause) {
    showToast!(requestError(cause, '导出工具失败'), true)
  }
}

const openImportDialog = (): void => {
  importForm.resetValidation()
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
  if (!importForm.validateForm()) return
  const toolData: unknown = JSON.parse(importJson.value)
  if (importing.value) return
  try {
    importing.value = true
    const resp = await fetch('/admin/api/custom-tool/import', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(toolData)
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!('工具已导入')
      showImportDialog.value = false
      loadTools()
    } else {
      showToast!(data.error || '导入失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '导入工具失败'), true)
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
  if (testing.value || saving.value || !testForm.validateForm()) return
  testing.value = true
  testResult.value = ''
  try {
    const testArgsJson: unknown = JSON.parse(testArgs.value)

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
    if (resp.ok && data.success) {
      testResult.value = data.result
    } else {
      testResult.value = '错误: ' + (data.error || '测试失败')
    }
  } catch (cause) {
    showToast!(requestError(cause, '工具测试失败'), true)
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
    <template v-if="!showEditor">
      <div class="page-header">
        <h1 id="tools-page-title" class="page-title" tabindex="-1">工具配置</h1>
        <p class="page-subtitle">管理内置工具与自定义工具</p>
      </div>

      <SlidingTabs v-model="activeTab" :items="toolTabs" label="工具类型" id-prefix="tool-type">
        <template #panel>
          <section v-if="activeTab === 'builtin'" class="builtin-tools">
            <div class="tools-toolbar">
              <label for="builtin-tool-search" class="form-label">搜索内置工具</label>
              <input id="builtin-tool-search" v-model="builtinFilter" class="form-input tools-search" type="search" placeholder="搜索名称或描述"
                     aria-label="搜索内置工具">
              <span class="tools-count">{{ enabledBuiltinCount }} / {{ builtinTools.length }} 已启用</span>
            </div>
            <p v-if="builtinLoading" class="empty-state">加载中...</p>
            <div v-else-if="builtinError" class="empty-state">
              <p>{{ builtinError }}</p><button class="btn btn-secondary" @click="loadBuiltins">重试</button>
            </div>
            <template v-else>
              <section v-for="group in builtinGroups" :key="group.category" class="builtin-group">
                <h3>{{ group.category }}工具 <span>{{ group.tools.length }}</span></h3>
                <div v-for="tool in group.tools" :key="tool.name"
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
              <label for="custom-tool-search" class="form-label">搜索自定义工具</label>
              <input id="custom-tool-search" v-model="customFilter" class="form-input tools-search" type="search" placeholder="搜索名称或描述"
                     aria-label="搜索自定义工具">
              <span class="tools-count">{{ enabledCustomCount }} / {{ tools.length }} 已启用</span>
              <div class="tool-actions">
                <button class="btn btn-secondary btn-sm" @click="openImportDialog">导入</button>
                <button class="btn btn-secondary btn-sm" @click="reloadTools">重新加载</button>
                <button class="btn btn-primary btn-sm" id="add-custom-tool" @click="openAddEditor">添加工具</button>
              </div>
            </div>
            <RequestError :message="toolsError" :busy="loading" :stale="toolsLoaded" @retry="loadTools"/>
            <p v-if="loading && !toolsLoaded" class="empty-state" role="status">加载中...</p>
            <p v-if="loading && toolsLoaded" role="status">刷新中…</p>
            <section v-if="toolsLoaded" class="builtin-group">
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
                            :id="`edit-custom-tool-${tool.id}`" @click="openEditEditor(tool)">编辑</button>
                    <button class="btn btn-danger btn-sm" :disabled="pendingTools.has(`custom:${tool.id}`)"
                            @click="deleteTool(tool.id)">{{ pendingTools.has(`custom:${tool.id}`) ? '处理中…' : '删除' }}</button>
                  </div>
                </div>
              </div>
              <p v-if="!visibleCustomTools.length" class="empty-state">{{ tools.length ? '暂无匹配的工具' : '暂无自定义工具' }}</p>
            </section>
          </section>

        </template>
      </SlidingTabs>

    </template>

    <!-- 独立的工具编辑视图，长表单使用页面滚动。 -->
    <section v-if="showEditor" class="tool-editor-page" aria-labelledby="tool-editor-title">
      <form ref="editorForm" class="tool-editor-form" novalidate @submit.prevent="saveTool">
        <div class="page-header tool-editor-heading">
          <div>
            <h1 id="tool-editor-title" class="page-title" tabindex="-1">{{ isEdit ? '编辑工具' : '添加工具' }}</h1>
            <p class="page-subtitle">{{ isEdit ? editTool.name : '填写工具信息、Lua 脚本和参数，并可在保存前执行测试。' }}</p>
          </div>
          <button type="button" class="btn btn-secondary" :disabled="saving || testing" @click="closeEditor">返回工具列表</button>
        </div>

        <div class="tool-editor-body">
          <h2 class="editor-section-title">基本信息</h2>
          <div class="form-row">
            <div class="form-group">
              <label class="form-label" for="tool-name">工具名称 *</label>
              <input v-bind="fieldAttrs('tool-name')" v-model="editTool.name" :disabled="isEdit" class="form-input"
                     placeholder="如: search_web, calculate">
            <FieldError field="tool-name" :message="errors['tool-name']"/>
            </div>
          </div>

          <div class="form-group">
            <label class="form-label" for="tool-description">工具描述 *</label>
            <input v-bind="fieldAttrs('tool-description')" v-model="editTool.description" class="form-input" placeholder="描述工具功能，供 LLM 理解何时调用">
            <FieldError field="tool-description" :message="errors['tool-description']"/>
          </div>

          <h2 class="editor-section-title">参数与脚本</h2>
          <div class="form-group">
            <label class="form-label" for="tool-parameters">参数定义 (JSON Schema)</label>
            <textarea v-bind="fieldAttrs('tool-parameters')" v-model="editTool.parameters" class="form-input code-input auto-height"
                      @input="autoResize"></textarea>
            <FieldError field="tool-parameters" :message="errors['tool-parameters']"/>
          </div>

          <div class="form-group">
            <label class="form-label" for="tool-script">Lua 脚本</label>
            <textarea v-bind="fieldAttrs('tool-script')" v-model="editTool.scriptContent" class="form-input code-input auto-height" spellcheck="false"
                      @input="autoResize"></textarea>
            <FieldError field="tool-script" :message="errors['tool-script']"/>
            <small class="form-hint">定义 run(args, ctx)；bot.send_message 可等待发送结果，bot.start_task 启动可选的 background 入口。测试不会真正发送。</small>
          </div>

          <!-- README 文档 -->
          <div class="form-group">
            <label class="form-label" for="tool-readme">说明文档 (Markdown)</label>
            <textarea v-bind="fieldAttrs('tool-readme')" v-model="editTool.readme" class="form-input auto-height" placeholder="作者、用法、联系方式等信息..."
                      style="min-height: 80px;" @input="autoResize"></textarea>
            <FieldError field="tool-readme" :message="errors['tool-readme']"/>
            <small class="form-hint">可选填写作者、使用示例、联系方式等，便于分享和查阅</small>
          </div>

          <!-- 测试区域 -->
          <div class="test-section">
            <div class="test-header">
              <span class="test-title">测试工具</span>
              <button type="button" :disabled="testing || saving" class="btn btn-secondary btn-sm" @click="testCurrentTool">
                {{ testing ? '执行中...' : '执行测试' }}
              </button>
            </div>
            <div class="test-content">
              <div class="test-input">
                <label for="tool-test-args">输入参数 (JSON)</label>
                <textarea v-bind="testForm.fieldAttrs('tool-test-args')" v-model="testArgs" class="form-input code-input auto-height" @input="autoResize"></textarea>
            <FieldError field="tool-test-args" :message="testForm.errors.value['tool-test-args']"/>
              </div>
              <div class="test-output">
                <span>输出结果</span>
                <pre class="result-box">{{ testResult || '点击执行测试查看结果...' }}</pre>
              </div>
            </div>
          </div>

          <div class="form-group checkbox-group">
            <label class="form-checkbox"><input v-model="editTool.enabled" :disabled="saving" type="checkbox">启用此工具（保存后生效）</label>
          </div>
        </div>

        <div class="tool-editor-footer">
          <button type="button" :disabled="saving || testing" class="btn btn-secondary" @click="closeEditor">取消</button>
          <button type="submit" :disabled="saving || testing" class="btn btn-primary">
            {{ saving ? '保存中...' : '保存' }}
          </button>
        </div>
      </form>
    </section>


    <ModalDialog v-if="editingBuiltin" class="dialog-overlay" labelledby="builtin-dialog-title" @close="closeBuiltin">
      <form class="dialog builtin-description-dialog" @submit.prevent="saveBuiltin">
        <div class="dialog-header">
          <h3 id="builtin-dialog-title">{{ editingBuiltin.name }}</h3>
          <button type="button" class="dialog-close" aria-label="关闭对话框" :disabled="builtinSaving" @click="closeBuiltin">×</button>
        </div>
        <div class="dialog-body">
          <label class="form-label" for="builtin-description">工具描述</label>
          <textarea id="builtin-description" v-model="editingBuiltin.description" :disabled="builtinSaving" class="form-input"
                    rows="7" @input="editingBuiltin.customDescription = true"></textarea>
        </div>
        <div class="dialog-footer">
          <button type="button" class="btn btn-secondary" :disabled="builtinSaving"
                  @click="editingBuiltin.description = editingBuiltin.defaultDescription; editingBuiltin.customDescription = false">恢复默认描述</button>
          <button type="button" class="btn btn-secondary" :disabled="builtinSaving" @click="closeBuiltin">取消</button>
          <button type="submit" class="btn btn-primary" :disabled="builtinSaving">{{ builtinSaving ? '保存中...' : '保存' }}</button>
        </div>
      </form>
    </ModalDialog>
    <!-- 导入对话框 -->
    <ModalDialog v-if="showImportDialog" class="dialog-overlay" labelledby="import-dialog-title" @close="closeImport">
      <div class="dialog" style="width: 600px;">
        <div class="dialog-header">
          <h3 id="import-dialog-title">导入工具</h3>
          <button class="dialog-close" aria-label="关闭对话框" @click="closeImport">×</button>
        </div>
        <div class="dialog-body">
          <div class="form-group">
            <label class="form-label" for="tool-import-file">上传 JSON 文件</label>
            <input id="tool-import-file" accept=".json" class="form-input" type="file" @change="handleFileUpload">
          </div>
          <div class="form-group">
            <label class="form-label" for="tool-import-json">或粘贴 JSON 内容</label>
            <textarea v-bind="importForm.fieldAttrs('tool-import-json')" v-model="importJson" class="form-input code-input" placeholder="粘贴工具 JSON 内容..."
                      spellcheck="false" style="min-height: 200px;"></textarea>
            <FieldError field="tool-import-json" :message="importForm.errors.value['tool-import-json']"/>
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
          <button class="btn btn-secondary" @click="closeImport">取消</button>
          <button :disabled="importing" class="btn btn-success" @click="importTool">
            {{ importing ? '导入中...' : '导入' }}
          </button>
        </div>
      </div>
    </ModalDialog>

    <!-- README 详情对话框 -->
    <ModalDialog v-if="showReadmeDialog" class="dialog-overlay" labelledby="readme-dialog-title" @close="showReadmeDialog = false">
      <div class="dialog" style="width: 700px; max-height: 80vh;">
        <div class="dialog-header">
          <h3 id="readme-dialog-title" tabindex="-1" data-modal-initial-focus>{{ currentToolName }}</h3>
          <button class="dialog-close" aria-label="关闭对话框" @click="showReadmeDialog = false">×</button>
        </div>
        <div class="dialog-body readme-content" v-html="currentReadme"></div>
        <div class="dialog-footer">
          <button class="btn btn-secondary" @click="showReadmeDialog = false">关闭</button>
        </div>
      </div>
    </ModalDialog>
  </div>
</template>

<style scoped>
.tool-editor-page { max-width: 1200px; min-width: 0; }
.tool-editor-form { min-width: 0; }
.tool-editor-heading { display: flex; justify-content: space-between; align-items: flex-start; gap: 16px; flex-wrap: wrap; }
.tool-editor-heading .page-subtitle { overflow-wrap: anywhere; }
.editor-section-title { font-size: 16px; margin: 24px 0 16px; padding-top: 16px; border-top: 1px solid var(--border); }
.editor-section-title:first-child { margin-top: 0; }
.tool-editor-footer { display: flex; justify-content: flex-end; gap: 12px; flex-wrap: wrap; padding: 20px 0; border-top: 1px solid var(--border); }
.tool-editor-page .auto-height { min-height: 120px; }
.tool-editor-page .test-input, .tool-editor-page .test-output { min-width: 0; }
.tool-editor-page .test-content { grid-template-columns: repeat(2, minmax(0, 1fr)); }
@media (max-width: 760px) {
  .tool-editor-page .test-content { grid-template-columns: 1fr; }
}

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
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}

.test-input label,
.test-output > span {
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
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 8px;
  background: var(--bg-secondary);
  position: sticky;
  bottom: 0;
}

@media (max-width: 600px) {
  .test-content { grid-template-columns: minmax(0, 1fr); }
  .dialog-header, .dialog-body, .dialog-footer { padding-inline: 16px; }
}
</style>
