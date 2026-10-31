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



const modelConfig = () => ({model: 'example', baseUrl: 'https://example.com/v1', apiKey: '', path: '/chat/completions',
  maxTokens: 1024, temperature: 0.7, topP: 1, timeoutSeconds: 90, maxRetries: 3, minConfidence: 0.6})
function observeFocus() {
  const focused = []
  document.getElementById = id => ({focus: () => focused.push(id)})
  return focused
}

test('QQ validation rejects invalid notation and overflow without a request, then submits large IDs as strings', async () => {
  const state = mount('access/AccessManager')
  await flush()
  const focused = observeFocus(), submitted = []
  globalThis.fetch = async (url, options) => {
    if (options?.method) submitted.push({url, body: options.body && JSON.parse(options.body)})
    return response(options?.method ? {success: true} : [])
  }
  assert.deepEqual(state.adminForm.errors.value, {})
  for (const value of ['', '1e9', '-42', '123.5', '18446744073709551616']) {
    state.newAdminQQ = value
    await state.addAdmin()
    assert.ok(state.adminForm.errors.value['admin-qq'])
  }
  assert.equal(submitted.length, 0)
  assert.equal(focused.at(-1), 'admin-qq')
  state.newAdminQQ = ' 9007199254740993 '
  await state.addAdmin()
  assert.equal(submitted[0].body.qq, '9007199254740993')
  await state.removeAdmin('9007199254740993')
  assert.ok(submitted.at(-1).url.endsWith('/9007199254740993'))
  state.newBlacklistQQ = '18446744073709551615'
  await state.addBlacklistEntry()
  assert.equal(submitted.at(-1).body.qq, '18446744073709551615')
})

test('OneBot validates only the selected transport and saves the exact QQ string', async () => {
  globalThis.fetch = async () => response({oneBotTransport: 'http', selfQQNumber: '42', qqHttpHost: 'http://localhost:3000', qqWebSocketHost: 'invalid hidden address'})
  const state = mount('onebot/OneBotConfig')
  await flush()
  const focused = observeFocus(), submitted = []
  globalThis.fetch = async (_url, options) => { submitted.push(JSON.parse(options.body)); return response({success: true}) }
  state.qqConfig.selfQQNumber = '9007199254740993'
  await state.saveQQConfig()
  assert.equal(submitted[0].selfQQNumber, '9007199254740993')
  state.qqConfig.oneBotTransport = 'websocket'
  await state.saveQQConfig()
  assert.equal(submitted.length, 1)
  assert.ok(state.errors['onebot-ws-host'])
  assert.equal(focused.at(-1), 'onebot-ws-host')
  state.qqConfig.qqWebSocketHost = 'wss://example.com/ws'
  await state.saveQQConfig()
  assert.equal(submitted.length, 2)
})

test('memory validation preserves invalid drafts and reports both numeric and dependent limits', async () => {
  const state = mount('conversation/MemoryConfig')
  await flush()
  state.loaded = true
  const focused = observeFocus()
  let posts = 0
  globalThis.fetch = async () => { posts++; return response({success: true}) }
  assert.deepEqual(state.errors, {})
  state.memoryConfig.contextWindowLimit = ''
  state.memoryConfig.memorySummaryBatchSize = 101
  state.memoryConfig.routerWindowKeepCount = 20
  state.memoryConfig.longTermRecallThreshold = 1
  await state.saveMemoryConfig()
  assert.equal(posts, 0)
  assert.equal(state.memoryConfig.contextWindowLimit, '')
  assert.equal(focused[0], 'memory-contextWindowLimit')
  for (const key of ['contextWindowLimit','memorySummaryBatchSize','routerWindowKeepCount','longTermRecallThreshold']) assert.ok(state.errors[`memory-${key}`])
  Object.assign(state.memoryConfig, {contextWindowLimit: 100, memorySummaryBatchSize: 50, routerWindowKeepCount: 10, longTermRecallThreshold: 0.65})
  await state.saveMemoryConfig()
  assert.equal(posts, 1)
})

test('model numeric validation runs before requests, allows fractional sampling, and uses unique IDs', async () => {
  const state = mount('llm/LLMModelForm', {name: 'executor', initialConfig: modelConfig()})
  observeFocus()
  let posts = 0
  globalThis.fetch = async () => { posts++; return response({success: true}) }
  state.llmConfig.timeoutSeconds = 0
  state.llmConfig.maxRetries = 1.5
  await state.saveLLMConfig()
  assert.equal(posts, 0)
  assert.ok(state.errors['model-executor-timeoutSeconds'])
  assert.ok(state.errors['model-executor-maxRetries'])
  assert.equal(state.fieldAttrs('model-executor-timeoutSeconds')['aria-invalid'], true)
  assert.match(state.fieldAttrs('model-executor-timeoutSeconds')['aria-describedby'], /-error/)
  Object.assign(state.llmConfig, {timeoutSeconds: 90, maxRetries: 0, temperature: 0.75, topP: 0.95})
  await state.saveLLMConfig()
  assert.equal(posts, 1)
  const fallback = mount('llm/LLMModelForm', {name: 'executorFallback', initialConfig: modelConfig()})
  assert.notEqual(state.fieldId('model'), fallback.fieldId('model'))
})

test('hidden model generation parameters are omitted from requests', async () => {
  const state = mount('llm/LLMModelForm', {name: 'embedding', initialConfig: {...modelConfig(), maxTokens: 'invalid hidden value'}})
  let submitted
  globalThis.fetch = async (_url, options) => { submitted = JSON.parse(options.body); return response({success: true}) }
  await state.saveLLMConfig()
  assert.equal('maxTokens' in submitted, false)
  assert.equal('temperature' in submitted, false)
})

test('tool JSON errors stay at the schema field and corrected drafts submit once', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  state.openAddEditor()
  state.editTool.name = 'example'
  state.editTool.description = 'description'
  state.editTool.parameters = '[]'
  const focused = observeFocus()
  let posts = 0
  globalThis.fetch = async (_url, options) => { if (options) posts++; return response(options ? {success: true} : []) }
  await state.saveTool()
  assert.equal(posts, 0)
  assert.equal(focused.at(-1), 'tool-parameters')
  assert.ok(state.errors['tool-parameters'])
  state.editTool.parameters = '{}'
  state.editTool.enabled = false
  await state.saveTool()
  assert.equal(posts, 1)
})

test('execution dependent limits identify the exact invalid field on submit', async () => {
  globalThis.fetch = async () => response({maxToolRounds: 8, routeWhileReplying: false, mcpToolPageSize: 10, mcpMaxToolsPerEnable: 4, mcpMaxToolsPerReply: 12})
  const state = mount('agent/ExecutionConfig')
  await flush()
  const focused = observeFocus()
  state.config.mcpMaxToolsPerEnable = 15
  await state.saveConfig()
  assert.ok(state.errors.mcpMaxToolsPerEnable)
  assert.equal(focused.at(-1), 'mcpMaxToolsPerEnable')
})
