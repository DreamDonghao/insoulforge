<script lang="ts" setup>
/**
 * @file CustomTools.vue
 * @brief 自定义工具管理组件
 */
import {computed, inject, nextTick, onMounted, reactive, ref, type Ref} from 'vue'
import {marked} from 'marked'
import type {ApiResponse} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

interface CustomTool {
  id: number
  name: string
  description: string
  parameters: string
  executorType: 'python' | 'http'
  executorConfig: string
  scriptContent: string
  readme: string
  enabled: boolean
}

const tools: Ref<CustomTool[]> = ref([])
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

// Python 配置
const showConfig: Ref<boolean> = ref(false)
const pythonPath: Ref<string> = ref('python3')
const savingConfig: Ref<boolean> = ref(false)

const defaultPythonScript = `# Python 工具脚本
import sys
import json

args = {}
if len(sys.argv) > 1:
    with open(sys.argv[1]) as f:
        args = json.load(f)

result = {"status": "ok", "args": args}
print(json.dumps(result, ensure_ascii=False))`

const editTool = reactive<CustomTool>({
  id: 0,
  name: '',
  description: '',
  parameters: '{"type":"object","properties":{},"required":[]}',
  executorType: 'python',
  executorConfig: '',
  scriptContent: defaultPythonScript,
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

const loadConfig = async (): Promise<void> => {
  try {
    const resp = await fetch('/admin/api/custom-tool-config')
    const data = await resp.json()
    if (data.pythonPath) {
      pythonPath.value = data.pythonPath
    }
  } catch {
    // ignore
  }
}

const saveConfig = async (): Promise<void> => {
  if (!pythonPath.value.trim()) {
    showToast!('请填写Python解释器路径', true)
    return
  }

  savingConfig.value = true
  try {
    const resp = await fetch('/admin/api/custom-tool-config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({pythonPath: pythonPath.value.trim()})
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('配置已保存')
      showConfig.value = false
    } else {
      showToast!(data.error || '保存失败', true)
    }
  } finally {
    savingConfig.value = false
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
    executorType: 'python',
    executorConfig: '',
    scriptContent: defaultPythonScript,
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
  if (editTool.executorType === 'python' && !editTool.scriptContent) {
    showToast!('请填写脚本内容', true)
    return
  }
  if (editTool.executorType === 'http' && !editTool.executorConfig) {
    showToast!('请填写执行配置', true)
    return
  }

  try {
    JSON.parse(editTool.parameters)
  } catch {
    showToast!('参数定义 JSON 格式错误', true)
    return
  }
  if (editTool.executorType === 'http') {
    try {
      JSON.parse(editTool.executorConfig)
    } catch {
      showToast!('执行配置 JSON 格式错误', true)
      return
    }
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

const toggleTool = async (id: number): Promise<void> => {
  const resp = await fetch(`/admin/api/custom-tool/${id}/toggle`, {method: 'POST'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('状态已切换')
    loadTools()
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
        executorConfig: editTool.executorConfig,
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

const dialogWidth = computed(() => editTool.executorType === 'python' ? '900px' : '600px')

onMounted(() => {
  loadTools()
  loadConfig()
})
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">自定义工具</h1>
      <p class="page-subtitle">添加自定义工具，扩展Bot能力</p>
    </div>

    <div class="card">
      <div class="card-header">
        <h3 class="card-title">工具管理</h3>
        <div style="display: flex; gap: 8px;">
          <button class="btn btn-secondary btn-sm" @click="showConfig = true">⚙️ Python配置</button>
          <button class="btn btn-secondary btn-sm" @click="openImportDialog">📥 导入</button>
          <button class="btn btn-primary btn-sm" @click="reloadTools">重新加载</button>
          <button class="btn btn-success btn-sm" @click="openAddDialog">添加工具</button>
        </div>
      </div>

      <div class="table-container">
        <table v-if="!loading">
          <thead>
          <tr>
            <th style="width: 60px">状态</th>
            <th style="width: 150px">工具名</th>
            <th>描述</th>
            <th style="width: 80px">类型</th>
            <th style="width: 140px">操作</th>
          </tr>
          </thead>
          <tbody>
          <tr v-for="tool in tools" :key="tool.id" class="tool-row" @click="showReadme(tool)">
            <td>
                <span :class="{ 'status-enabled': tool.enabled, 'status-disabled': !tool.enabled }">
                  {{ tool.enabled ? '启用' : '禁用' }}
                </span>
            </td>
            <td><code>{{ tool.name }}</code></td>
            <td>{{ tool.description }}</td>
            <td>
              <span :class="'tag-' + tool.executorType" class="tag">{{ tool.executorType }}</span>
            </td>
            <td style="white-space: nowrap;" @click.stop>
              <button :class="tool.enabled ? 'btn-warning' : 'btn-success'" class="btn btn-sm"
                      @click="toggleTool(tool.id)">
                {{ tool.enabled ? '禁用' : '启用' }}
              </button>
              <button v-if="tool.executorType === 'python'" class="btn btn-secondary btn-sm" style="margin-left: 4px;"
                      @click="exportTool(tool.id, tool.name)">导出
              </button>
              <button class="btn btn-primary btn-sm" style="margin-left: 4px;" @click="openEditDialog(tool)">编辑
              </button>
              <button class="btn btn-danger btn-sm" style="margin-left: 4px;" @click="deleteTool(tool.id)">删除</button>
            </td>
          </tr>
          </tbody>
        </table>

        <div v-if="loading" class="empty-state">
          <p>加载中...</p>
        </div>
        <div v-else-if="tools.length === 0" class="empty-state">
          <div class="empty-icon">🔧</div>
          <p>暂无自定义工具</p>
          <button class="btn btn-success" @click="openAddDialog">添加工具</button>
        </div>
      </div>
    </div>

    <!-- Python 配置对话框 -->
    <div v-if="showConfig" class="dialog-overlay">
      <div class="dialog" style="width: 500px;">
        <div class="dialog-header">
          <h3>Python 解释器配置</h3>
          <button class="dialog-close" @click="showConfig = false">×</button>
        </div>
        <div class="dialog-body">
          <div class="form-group">
            <label class="form-label">Python 解释器路径</label>
            <input v-model="pythonPath" class="form-input" placeholder="例如: python3 或 /home/user/myenv/bin/python">
            <small class="form-hint">
              默认使用系统 python3。如需使用第三方包，请先创建虚拟环境并在此配置路径。
            </small>
          </div>
          <div class="config-example">
            <strong>创建虚拟环境示例：</strong>
            <pre>python3 -m venv ~/my_bot_env
source ~/my_bot_env/bin/activate
pip install requests numpy  # 安装需要的包</pre>
            <p>然后配置路径为: <code>~/my_bot_env/bin/python</code> 或
              <code>/home/你的用户名/my_bot_env/bin/python</code></p>
          </div>
        </div>
        <div class="dialog-footer">
          <button class="btn btn-secondary" @click="showConfig = false">取消</button>
          <button :disabled="savingConfig" class="btn btn-success" @click="saveConfig">
            {{ savingConfig ? '保存中...' : '保存' }}
          </button>
        </div>
      </div>
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
  "scriptContent": "import json\n..."
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
      <div :style="{ width: dialogWidth }" class="dialog">
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
            <div class="form-group" style="width: 120px;">
              <label class="form-label">执行类型</label>
              <select v-model="editTool.executorType" class="form-input">
                <option value="python">Python</option>
                <option value="http">HTTP</option>
              </select>
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

          <!-- Python 脚本编辑器 -->
          <div v-if="editTool.executorType === 'python'" class="form-group">
            <label class="form-label">Python 脚本</label>
            <textarea v-model="editTool.scriptContent" class="form-input code-input auto-height" spellcheck="false"
                      @input="autoResize"></textarea>
            <small class="form-hint">参数通过 sys.argv[1] 传入 JSON 文件路径</small>
          </div>

          <!-- README 文档 -->
          <div class="form-group">
            <label class="form-label">说明文档 (Markdown)</label>
            <textarea v-model="editTool.readme" class="form-input auto-height" placeholder="作者、用法、联系方式等信息..."
                      style="min-height: 80px;" @input="autoResize"></textarea>
            <small class="form-hint">可选填写作者、使用示例、联系方式等，便于分享和查阅</small>
          </div>

          <!-- HTTP 配置 -->
          <div v-if="editTool.executorType === 'http'" class="form-group">
            <label class="form-label">HTTP 配置 (JSON)</label>
            <textarea v-model="editTool.executorConfig" class="form-input code-input auto-height"
                      @input="autoResize"></textarea>
            <small class="form-hint">示例: {"url": "http://api.example.com", "method": "POST"}</small>
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
            <label>
              <input v-model="editTool.enabled" type="checkbox"> 启用此工具
            </label>
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
.status-enabled {
  color: var(--success);
  font-weight: 500;
}

.status-disabled {
  color: var(--danger);
  font-weight: 500;
}

.tag {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 500;
}

.tag-python {
  background: #3776ab;
  color: white;
}

.tag-http {
  background: var(--success-btn);
  color: white;
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
  border-radius: 12px;
  max-width: 95vw;
  max-height: 90vh;
  overflow-y: auto;
  box-shadow: var(--shadow-md);
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

.tool-row {
  cursor: pointer;
  transition: background-color 0.15s;
}

.tool-row:hover {
  background-color: var(--row-hover);
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
  border-radius: 4px;
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 13px;
}

.readme-content pre {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 12px 16px;
  border-radius: 6px;
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

.checkbox-group label {
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  font-size: 13px;
}

.config-example {
  margin-top: 12px;
  padding: 12px;
  background: var(--bg-secondary);
  border-radius: 6px;
  font-size: 12px;
}

.config-example pre {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 8px 12px;
  border-radius: 4px;
  font-size: 11px;
  margin: 8px 0;
  overflow-x: auto;
}

.config-example p {
  margin: 8px 0 0 0;
  color: var(--text-secondary);
}

.import-example {
  margin-top: 12px;
  padding: 12px;
  background: var(--bg-secondary);
  border-radius: 6px;
  font-size: 12px;
}

.import-example pre {
  background: var(--code-bg);
  color: var(--code-text);
  padding: 8px 12px;
  border-radius: 4px;
  font-size: 11px;
  margin: 8px 0;
  overflow-x: auto;
}

.test-section {
  margin-top: 16px;
  border: 1px solid var(--border);
  border-radius: 8px;
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
  border-radius: 4px;
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