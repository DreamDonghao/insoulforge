import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {beforeEach, afterEach, test} from 'node:test'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const require = createRequire(import.meta.url)
const modules = new Map()
function load(path) {
  if (modules.has(path)) return modules.get(path).exports
  const module = {exports: {}}
  modules.set(path, module)
  const source = readFileSync(path, 'utf8')
  const code = path.endsWith('.vue') ? compileScript(parse(source).descriptor, {id: path}).content : source
  const {outputText} = ts.transpileModule(code, {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
  })
  new Function('require', 'module', 'exports', outputText)(name => {
    if (name === 'vue') return vue
    if (!name.startsWith('.')) return require(name)
    if (name.endsWith('.vue')) return {default: {render: () => null}}
    return load(resolve(dirname(path), `${name}.ts`))
  }, module, module.exports)
  if (path.endsWith('.vue')) module.exports.default.render = () => null
  return module.exports
}
const renderer = vue.createRenderer({
  createElement: () => ({}), createText: () => ({}), createComment: () => ({}),
  insert() {}, remove() {}, setText() {}, setElementText() {}, patchProp() {},
  parentNode: () => null, nextSibling: () => null
})
const response = data => ({ok: true, status: 200, headers: new Headers({'content-type': 'application/json'}), json: async () => data})
const flush = async () => { await vue.nextTick(); await new Promise(resolve => setImmediate(resolve)) }
let apps, originals, guards, confirmResult, confirmations, events, toasts
beforeEach(() => {
  apps = []
  guards = new Set()
  confirmResult = false
  confirmations = 0
  events = new EventTarget()
  toasts = []
  originals = Object.fromEntries(['window', 'document', 'fetch', 'WebSocket'].map(key => [key, globalThis[key]]))
  globalThis.window = {
    confirm() { confirmations++; return confirmResult },
    addEventListener: (...args) => events.addEventListener(...args),
    removeEventListener: (...args) => events.removeEventListener(...args)
  }
  globalThis.document = {body: {style: {}}, querySelectorAll: () => []}
  globalThis.WebSocket = {OPEN: 1}
  globalThis.fetch = async url => {
    if (url === '/admin/api/character-image') return {...response({}), ok: false, status: 404}
    if (url === '/admin/api/prompts') return response({executor_system: 'original A', router_system: 'original B'})
    return response([])
  }
})
afterEach(() => {
  apps.forEach(app => app.unmount())
  for (const [key, value] of Object.entries(originals)) {
    if (value === undefined) delete globalThis[key]
    else globalThis[key] = value
  }
})
function mount(page, props = {}) {
  const app = renderer.createApp(load(resolve(root, `src/features/${page}.vue`)).default, props)
  app.provide('leaveGuards', guards)
  app.provide('showToast', (...args) => toasts.push(args))
  app.provide('qqConfig', {})
  app.provide('wsConnected', vue.ref(false))
  app.provide('ws', {get: () => null})
  app.mount({})
  apps.push(app)
  return app._instance.setupState
}

test('prompt switching preserves a rejected draft and discards only after confirmation', async () => {
  const state = mount('llm/PromptEditor')
  await flush()
  state.promptContent = 'unsaved A'
  state.selectPrompt('router_system')
  assert.equal(state.selectedPrompt, 'executor_system')
  assert.equal(state.promptContent, 'unsaved A')
  assert.equal(confirmations, 1)
  assert.equal([...guards][0].dirty(), true)
  confirmResult = true
  state.selectPrompt('router_system')
  assert.equal(state.promptContent, 'original B')
  assert.equal([...guards][0].dirty(), false)
})

test('pending prompt save updates only its submitted key and content after switching', async () => {
  const state = mount('llm/PromptEditor')
  await flush()
  let finish, submitted
  globalThis.fetch = async (url, options) => {
    submitted = JSON.parse(options.body)
    return new Promise(resolve => { finish = resolve })
  }
  state.promptContent = 'submitted A'
  const pending = state.savePrompt()
  await state.savePrompt()
  assert.equal([...guards][0].busy(), true)
  confirmResult = true
  state.selectPrompt('router_system')
  state.promptContent = 'new unsaved B'
  finish(response({success: true}))
  await pending
  assert.deepEqual(submitted, {key: 'executor_system', content: 'submitted A'})
  assert.equal(state.prompts.executor_system, 'submitted A')
  assert.equal(state.prompts.router_system, 'original B')
  assert.equal(state.promptContent, 'new unsaved B')
  assert.equal([...guards][0].dirty(), true)
})

test('failed prompt saves retain drafts; success only marks the submitted revision saved', async () => {
  const state = mount('llm/PromptEditor')
  await flush()
  state.promptContent = 'draft'
  globalThis.fetch = async () => { throw new Error('offline') }
  await state.savePrompt()
  assert.equal(state.promptContent, 'draft')
  assert.equal(state.prompts.executor_system, 'original A')
  assert.equal([...guards][0].dirty(), true)
  let finish
  globalThis.fetch = () => new Promise(resolve => { finish = resolve })
  const pending = state.savePrompt()
  state.promptContent = 'typed while saving'
  finish(response({success: true}))
  await pending
  assert.equal(state.prompts.executor_system, 'draft')
  assert.equal(state.promptContent, 'typed while saving')
  assert.equal([...guards][0].dirty(), true)
})

test('model save keeps edits made during a request and saves only the submitted revision', async () => {
  const state = mount('llm/LLMModelForm', {name: 'executor', initialConfig: {model: 'old', maxTokens: 1024, temperature: 0.7, topP: 1, timeoutSeconds: 90, maxRetries: 3}})
  state.llmConfig.model = 'submitted'
  let finish
  globalThis.fetch = () => new Promise(resolve => { finish = resolve })
  const pending = state.saveLLMConfig()
  state.llmConfig.model = 'later edit'
  finish(response({success: true}))
  await pending
  assert.equal(state.llmConfig.model, 'later edit')
  assert.equal(state.dirty, true)
})

test('model switch checks every form, including the fallback, and blocks while saving', async () => {
  const state = mount('llm/LLMConfig')
  await flush()
  state.modelForms = [{dirty: false, saving: false}, {dirty: true, saving: false}]
  state.selectModel('image')
  assert.equal(state.selectedLLM, 'router')
  confirmResult = true
  state.selectModel('image')
  assert.equal(state.selectedLLM, 'image')
  state.modelForms = [{dirty: false, saving: true}]
  state.selectModel('memory')
  assert.equal(state.selectedLLM, 'image')
})

test('tool close rejects losing changes; successful save closes without asking to discard', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  state.openAddEditor()
  state.editTool.name = 'example'
  state.editTool.description = 'description'
  state.closeEditor()
  assert.equal(state.showEditor, true)
  globalThis.fetch = async url => response(url === '/admin/api/custom-tools' ? [] : {success: true})
  await state.saveTool()
  assert.equal(state.showEditor, false)
  assert.equal(confirmations, 1)
})

test('conversation editors require discard confirmation and are clean after saving', async () => {
  globalThis.fetch = async (url, options) => response(options ? {success: true} :
    url.endsWith('/prompt') ? {additionalPrompt: 'original'} :
    url.includes('/memory/') ? {memory: 'aliases'} : [])
  const state = mount('conversation/GroupManager')
  await flush()
  await state.viewMemory('42', 'Group')
  state.groupMemory = 'changed aliases'
  state.closeMemory()
  assert.equal(state.memoryGroupId, '42')
  await state.saveMemory()
  state.closeMemory()
  assert.equal(state.memoryGroupId, null)
  await state.openPrompt({groupId: '42'})
  state.promptDraft = 'changed prompt'
  state.closePrompt()
  assert.ok(state.promptGroup)
  await state.savePrompt()
  state.closePrompt()
  assert.equal(state.promptGroup, null)
})

test('browser unload warns only for drafts or pending saves, and cleans up on unmount', async () => {
  const state = mount('llm/PromptEditor')
  await flush()
  const unload = () => {
    const event = new Event('beforeunload', {cancelable: true})
    events.dispatchEvent(event)
    return event.defaultPrevented
  }
  assert.equal(unload(), false)
  state.promptContent = 'draft'
  assert.equal(unload(), true)
  state.promptContent = 'original A'
  state.saving = true
  assert.equal(unload(), true)
  apps.pop().unmount()
  assert.equal(guards.size, 0)
  assert.equal(unload(), false)
})

test('builtin and import dialogs keep unsaved content when discard is rejected', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  const original = {name: 'example', description: 'original', customDescription: false}
  state.builtinTools = [original]
  state.editingBuiltin = {...original, description: 'edited', customDescription: true}
  state.closeBuiltin()
  assert.equal(state.editingBuiltin.description, 'edited')
  confirmResult = true
  state.closeBuiltin()
  assert.equal(state.editingBuiltin, null)
  state.showImportDialog = true
  state.importJson = '{"name":"draft"}'
  confirmResult = false
  state.closeImport()
  assert.equal(state.showImportDialog, true)
  assert.equal(state.importJson, '{"name":"draft"}')
})

test('tool editor returns to the filtered custom list only after accepting draft discard', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  state.customFilter = 'example'
  state.openEditEditor({id: 7, name: 'example', description: 'original', parameters: '{}', executorType: 'lua', scriptContent: 'return 1', readme: '', enabled: true})
  state.editTool.description = 'unsaved'
  state.closeEditor()
  assert.equal(state.showEditor, true)
  assert.equal(state.editTool.description, 'unsaved')
  confirmResult = true
  state.closeEditor()
  assert.equal(state.showEditor, false)
  assert.equal(state.activeTab, 'custom')
  assert.equal(state.customFilter, 'example')
})

test('tool editor keeps failed saves and blocks leaving while a test is running', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  state.openAddEditor()
  state.editTool.name = 'example'
  state.editTool.description = 'description'
  globalThis.fetch = async () => { throw new Error('offline') }
  await state.saveTool()
  assert.equal(state.showEditor, true)
  assert.equal(state.editTool.name, 'example')
  let finish, requests = 0
  globalThis.fetch = () => { requests++; return new Promise(resolve => { finish = resolve }) }
  const pending = state.testCurrentTool()
  await state.testCurrentTool()
  state.closeEditor()
  assert.equal(state.showEditor, true)
  assert.equal(requests, 1)
  assert.equal([...guards][0].busy(), true)
  finish(response({success: true, result: 'test complete'}))
  await pending
  assert.equal(state.testResult, 'test complete')
  assert.equal([...guards][0].busy(), false)
})

for (const [page, key, save, data, field, changed] of [
  ['agent/ExecutionConfig', 'config', 'saveConfig', {maxToolRounds: 8, routeWhileReplying: false, mcpToolPageSize: 10, mcpMaxToolsPerEnable: 4, mcpMaxToolsPerReply: 12}, 'maxToolRounds', 9],
  ['conversation/MemoryConfig', 'memoryConfig', 'saveMemoryConfig', {contextWindowLimit: 100}, 'contextWindowLimit', 101],
  ['onebot/OneBotConfig', 'qqConfig', 'saveQQConfig', {oneBotTransport: 'http', selfQQNumber: '42', qqHttpHost: 'http://localhost:3000', botName: 'original'}, 'botName', 'changed']
]) {
  test(`${page}: leave protection follows loaded and submitted revisions and survives save failure`, async () => {
    globalThis.fetch = async () => response(data)
    const state = mount(page)
    await flush()
    const guard = [...guards][0]
    assert.ok(guard)
    assert.equal(guard.dirty(), false)
    state[key][field] = changed
    assert.equal(guard.dirty(), true)
    globalThis.fetch = async () => { throw new TypeError('offline') }
    await state[save]()
    assert.equal(guard.dirty(), true)
    assert.equal(guard.busy(), false)
    let finish
    globalThis.fetch = () => new Promise(resolve => { finish = resolve })
    const saving = state[save]()
    assert.equal(guard.busy(), true)
    state[key][field] = typeof changed === 'number' ? changed + 1 : 'newer draft'
    finish(response({success: true}))
    await saving
    assert.equal(guard.busy(), false)
    assert.equal(guard.dirty(), true)
    state[key][field] = changed
    assert.equal(guard.dirty(), false)
    state[key][field] = typeof changed === 'number' ? changed + 1 : 'final draft'
    globalThis.fetch = async () => response({success: true})
    await state[save]()
    assert.equal(guard.dirty(), false)
    apps.at(-1).unmount()
    assert.equal(guards.size, 0)
  })
}

test('MCP dialog protects every draft field and header row; rejected closes and failed saves keep drafts', async () => {
  const server = {name: 'example', description: '', url: 'https://example.com/mcp', token: '', headers: {}, enabled: true, timeoutSeconds: 30}
  globalThis.fetch = async () => response({servers: [server], status: []})
  const state = mount('tools/McpServers')
  await flush()
  const guard = [...guards][0]
  state.open(0)
  assert.equal(guard.dirty(), false)
  state.closeDialog()
  assert.equal(state.dialog, false)
  assert.equal(confirmations, 0)
  for (const mutate of [
    () => { state.draft.description = 'draft' },
    () => { state.draft.token = 'fixture' },
    () => { state.draft.timeoutSeconds = 45 },
    () => { state.headerRows.push({name: 'X-Test', value: ''}) }
  ]) {
    state.open(0)
    mutate()
    assert.equal(guard.dirty(), true)
    state.closeDialog()
    assert.equal(state.dialog, true)
  }
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.save()
  assert.equal(state.dialog, true)
  assert.equal(guard.dirty(), true)
  let finish
  globalThis.fetch = (_url, options) => options?.method === 'POST' ? new Promise(resolve => { finish = resolve }) : Promise.resolve(response({servers: [server], status: []}))
  const saving = state.save()
  assert.equal(guard.busy(), true)
  confirmResult = true
  state.closeDialog()
  assert.equal(state.dialog, true)
  finish(response({success: true}))
  await saving
  assert.equal(state.dialog, false)
  assert.equal(guard.dirty(), false)
  state.open(null)
  state.headerRows.push({name: '', value: ''})
  assert.equal(guard.dirty(), true)
  state.closeDialog()
  assert.equal(state.dialog, false)
})

for (const page of ['tools/EmojiManager', 'conversation/PeopleManager', 'conversation/GroupManager']) {
  test(`${page}: inline drafts protect cancellation, switching and page unload`, async () => {
    const state = mount(page)
    await flush()
    let cancel, switchEditor, draft, current
    if (page.endsWith('EmojiManager')) {
      state.startEdit({res_id: '42', summary: 'original'})
      state.editingDesc = 'draft'
      cancel = () => state.cancelEdit()
      switchEditor = () => state.startEdit({res_id: '99', summary: 'other'})
      draft = () => state.editingDesc
      current = () => state.editingId
    } else if (page.endsWith('PeopleManager')) {
      state.selected = '42'
      state.detail = {affinity: 0, profile: [], relationship: [], recent_interactions: [], pending_entries: []}
      state.beginEdit()
      state.draftText = 'draft'
      cancel = () => state.cancelEdit()
      switchEditor = () => state.selectPerson('99')
      draft = () => state.draftText
      current = () => state.editing ? state.selected : ''
    } else {
      state.selectedGroup = '42'
      state.startEdit({id: 42, content: 'original'})
      state.editContent = 'draft'
      cancel = () => state.cancelEdit()
      switchEditor = () => state.backToList()
      draft = () => state.editContent
      current = () => String(state.editingId ?? '')
    }
    const guard = [...guards][0]
    assert.equal(guard.dirty(), true)
    cancel()
    await switchEditor()
    assert.equal(draft(), 'draft')
    assert.equal(current(), '42')
    const unload = new Event('beforeunload', {cancelable: true})
    events.dispatchEvent(unload)
    assert.equal(unload.defaultPrevented, true)
    confirmResult = true
    cancel()
    assert.equal(guard.dirty(), false)
  })
}

test('MCP response body timeout releases navigation, preserves draft and requires reconciliation before resubmitting', async t => {
  t.mock.timers.enable({apis: ['setTimeout']})
  globalThis.fetch = async () => response({servers: [], status: []})
  const state = mount('tools/McpServers')
  await flush()
  state.open(null)
  state.draft.name = 'fixture'
  state.draft.url = 'https://example.com/mcp'
  const guard = [...guards][0]
  let signal, posts = 0
  globalThis.fetch = async (_url, options) => {
    posts++
    signal = options.signal
    return {ok: true, json: () => new Promise(() => {})}
  }
  const saving = state.save()
  await vue.nextTick()
  assert.equal(guard.busy(), true)
  t.mock.timers.tick(15000)
  await saving
  assert.equal(signal.aborted, true)
  assert.equal(guard.busy(), false)
  assert.equal(guard.dirty(), true)
  assert.equal(state.draft.name, 'fixture')
  assert.equal(state.dialog, true)
  assert.equal(state.needsReload, true)
  await state.save()
  assert.equal(posts, 1)
  globalThis.fetch = async () => response({servers: [], status: []})
  await state.load()
  assert.equal(state.needsReload, false)
  assert.equal(state.draft.name, 'fixture')
  confirmResult = true
  state.closeDialog()
  assert.equal(state.dialog, false)
})
