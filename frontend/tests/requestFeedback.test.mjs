import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {beforeEach, afterEach, test} from 'node:test'
import {compileScript, compileTemplate, parse} from 'vue/compiler-sfc'
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


for (const [page, load, dataKey, errorKey, url] of [
  ['access/AccessManager', 'loadAdmins', 'admins', 'adminError', '/admin/api/admins'],
  ['access/AccessManager', 'loadBlacklist', 'blacklist', 'blacklistError', '/admin/api/blacklist'],
  ['tools/EmojiManager', 'loadEmojis', 'emojis', 'loadError', '/admin/api/emojis'],
  ['tools/CustomTools', 'loadTools', 'tools', 'toolsError', '/admin/api/custom-tools'],
  ['conversation/GroupManager', 'loadGroups', 'groups', 'groupsError', '/admin/api/groups']
]) {
  test(`${page}/${load}: failures preserve previous data; only a valid response clears the error`, async () => {
    const state = mount(page)
    await flush()
    const original = [{qq: '42', groupId: '42', name: 'example'}]
    state[dataKey] = original
    const snapshot = JSON.stringify(state[dataKey])
    const previousFetch = globalThis.fetch
    for (const fail of [
      async () => { throw new TypeError('Failed to fetch') },
      async () => ({...response({error: 'service unavailable'}), ok: false, status: 503}),
      async () => response({unexpected: true})
    ]) {
      globalThis.fetch = (path, options) => path === url ? fail() : previousFetch(path, options)
      await state[load]()
      assert.equal(JSON.stringify(state[dataKey]), snapshot)
      assert.ok(state[errorKey])
    }
    globalThis.fetch = (path, options) => path === url ? Promise.resolve(response([])) : previousFetch(path, options)
    await state[load]()
    assert.equal(state[errorKey], '')
    assert.equal(state[dataKey].length, 0)
  })
}

for (const [page, load, save, key, data] of [
  ['conversation/MemoryConfig', 'loadConfig', 'saveMemoryConfig', 'memoryConfig', {contextWindowLimit: 123, affinityEnabled: false}],
  ['onebot/OneBotConfig', 'refreshActiveTransport', 'saveQQConfig', 'qqConfig', {oneBotTransport: 'http', botName: 'loaded', accessToken: 'fixture', selfQQNumber: '42', qqHttpHost: 'http://localhost:3000'}]
]) {
  test(`${page}: failed initial load prevents saving defaults; retry succeeds and failed saves preserve input`, async () => {
    let posts = 0
    globalThis.fetch = async (_url, options) => {
      if (options?.method === 'POST') posts++
      throw new TypeError('offline')
    }
    const state = mount(page)
    await flush()
    assert.equal(state.loaded, false)
    assert.ok(state.loadError)
    await state[save]()
    assert.equal(posts, 0)
    globalThis.fetch = async () => response(data)
    await state[load]()
    assert.equal(state.loaded, true)
    assert.equal(state.loadError, '')
    assert.equal(state[key][Object.keys(data)[0]], Object.values(data)[0])
    const snapshot = JSON.stringify(state[key])
    globalThis.fetch = async () => { throw new TypeError('offline') }
    await state[save]()
    assert.equal(state.saving, false)
    assert.equal(JSON.stringify(state[key]), snapshot)
    assert.equal(toasts.at(-1)[1], true)
  })
}

test('model configuration load errors have a retry state which clears after successful loading', async () => {
  globalThis.fetch = async () => ({...response({error: 'unavailable'}), ok: false, status: 503})
  const state = mount('llm/LLMConfig')
  await flush()
  assert.ok(state.loadError)
  assert.equal(state.loading, false)
  globalThis.fetch = async () => response({router: {model: 'router'}})
  await state.loadConfigs()
  assert.equal(state.loadError, '')
  assert.equal(state.llmConfigs.router.model, 'router')
})

test('failed prompt loads cannot save an empty prompt and can be retried in place', async () => {
  globalThis.fetch = async () => { throw new TypeError('offline') }
  const state = mount('llm/PromptEditor')
  await flush()
  assert.ok(state.promptLoadError)
  let saves = 0
  globalThis.fetch = async () => { saves++; return response({success: true}) }
  await state.savePrompt()
  assert.equal(saves, 0)
  globalThis.fetch = async () => response({executor_system: 'loaded'})
  await state.loadPrompts()
  assert.equal(state.promptLoadError, '')
  assert.equal(state.promptContent, 'loaded')
})

test('memory load errors block saving, task and memory failures remain distinct from empty results', async () => {
  const state = mount('conversation/GroupManager')
  await flush()
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.viewMemory('42', 'Group')
  assert.ok(state.memoryError)
  let saved = false
  globalThis.fetch = async () => { saved = true; return response({success: true}) }
  await state.saveMemory()
  assert.equal(saved, false)
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.viewTasks('42', 'Group')
  assert.ok(state.tasksError)
  await state.viewLongTermMemory('42', 'Group')
  assert.ok(state.ltmError)
})

test('failed long-term memory pagination retains both the previous page number and data', async () => {
  const state = mount('conversation/GroupManager')
  await flush()
  globalThis.fetch = async () => response({items: [{id: 1, content: 'kept'}], total: 40})
  await state.viewLongTermMemory('42', 'Group')
  globalThis.fetch = async () => { throw new TypeError('offline') }
  state.changeLtmPage(1)
  await flush()
  assert.equal(state.ltmPage, 0)
  assert.equal(state.ltmList[0].content, 'kept')
  assert.ok(state.ltmError)
})

test('tool import network failures are not misreported as invalid JSON', async () => {
  const state = mount('tools/CustomTools')
  await flush()
  state.importJson = '{"name":"example"}'
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.importTool()
  assert.match(toasts.at(-1)[0], /导入工具失败/)
  assert.equal(state.importing, false)
  assert.equal(state.importJson, '{"name":"example"}')
})

test('error notifications persist, deduplicate and survive success notifications until dismissed', t => {
  t.mock.timers.enable({apis: ['setTimeout']})
  const toast = load(resolve(root, 'src/composables/useToast.ts')).useToast()
  toast.notifications.value.forEach(item => toast.dismissToast(item.id))
  toast.showToast('save failed', true)
  toast.showToast('save failed', true)
  toast.showToast('other failure', true)
  toast.showToast('saved')
  assert.equal(toast.notifications.value.length, 3)
  t.mock.timers.tick(3001)
  assert.deepEqual(toast.notifications.value.map(item => item.message), ['save failed', 'other failure'])
  toast.notifications.value.forEach(item => toast.dismissToast(item.id))
  assert.equal(toast.notifications.value.length, 0)
})

const deferred = () => {
  let resolve, reject
  const promise = new Promise((res, rej) => { resolve = res; reject = rej })
  return {promise, resolve, reject}
}

test('message save snapshots its target and content, prevents duplicates and preserves newer edits', async () => {
  window.confirm = () => true
  const state = mount('conversation/GroupManager')
  await flush()
  state.selectedGroup = '42'
  state.chatRecords = [{id: 1, content: 'A'}, {id: 2, content: 'B'}]
  state.startEdit(state.chatRecords[0])
  state.editContent = 'saved A'
  const pending = deferred()
  const requests = []
  globalThis.fetch = (url, options) => { requests.push({url, options}); return pending.promise }
  const save = state.saveEdit()
  assert.equal(state.editSaving, true)
  state.startEdit(state.chatRecords[1])
  state.editContent = 'draft B'
  await state.saveEdit()
  assert.equal(requests.length, 1)
  assert.equal(requests[0].url, '/admin/api/chat-record/1')
  assert.deepEqual(JSON.parse(requests[0].options.body), {content: 'saved A'})
  pending.resolve(response({success: true}))
  await save
  assert.deepEqual(state.chatRecords.map(record => record.content), ['saved A', 'B'])
  assert.equal(state.editingId, 2)
  assert.equal(state.editContent, 'draft B')
  assert.equal(state.editSaving, false)
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.saveEdit()
  assert.equal(state.editSaving, false)
  assert.equal(state.editContent, 'draft B')
  assert.equal(state.editingId, 2)
  globalThis.fetch = async () => response({success: true})
  await state.saveEdit()
  assert.equal(state.chatRecords[1].content, 'draft B')
  assert.equal(state.editingId, null)
})

test('chat load failure stays distinct from a genuine empty result and supports retry', async () => {
  const state = mount('conversation/GroupManager')
  await flush()
  globalThis.fetch = async () => { throw new TypeError('offline') }
  await state.selectGroup('42', 'test')
  assert.ok(state.chatError)
  assert.equal(state.chatRecords.length, 0)
  assert.equal(state.chatLoading, false)
  const {descriptor} = parse(readFileSync(resolve(root, 'src/features/conversation/GroupManager.vue'), 'utf8'))
  assert.match(descriptor.template.content, /v-if="!chatError && chatRecords.length === 0"/)
  globalThis.fetch = async () => response([])
  await state.selectGroup('42', 'test')
  assert.equal(state.chatError, '')
  assert.equal(state.chatRecords.length, 0)
})

test('log filter changes supersede pending loads and pagination, and ignore stale responses after leaving', async t => {
  const oldLocation = globalThis.location
  globalThis.location = {protocol: 'http:', host: 'localhost'}
  t.after(() => { if (oldLocation === undefined) delete globalThis.location; else globalThis.location = oldLocation })
  globalThis.WebSocket = class {
    static OPEN = 1
    readyState = 1
    send() {}
    close() {}
  }
  window.requestAnimationFrame = () => 1
  window.cancelAnimationFrame = () => {}
  window.clearTimeout = () => {}
  globalThis.fetch = async url => response(url === '/admin/api/groups' ? [] : {entries: [], hasMore: false})
  const state = mount('diagnostics/LogViewer')
  await flush()
  const requests = []
  globalThis.fetch = (url, options) => {
    const pending = deferred()
    requests.push({url, signal: options.signal, ...pending})
    return pending.promise
  }
  const all = state.refresh()
  state.level = 'warn'
  const warn = state.refresh()
  assert.equal(requests[0].signal.aborted, true)
  assert.match(requests[1].url, /level=warn/)
  requests[0].resolve(response({entries: [{id: 1, level: 'info'}], hasMore: false}))
  await all
  assert.equal(state.loading, true)
  assert.deepEqual(state.entries, [])
  requests[1].resolve(response({entries: [{id: 2, level: 'warn'}], hasMore: true, nextBeforeId: 2}))
  await warn
  assert.equal(state.entries[0].id, 2)
  const older = state.loadOlder()
  await state.loadOlder()
  assert.equal(requests.length, 3)
  assert.match(requests[2].url, /beforeId=2/)
  state.level = 'error'
  const errors = state.refresh()
  requests[2].reject(new TypeError('old failure'))
  await older
  assert.equal(state.loadError, '')
  assert.equal(state.loading, true)
  requests[3].resolve(response({entries: [{id: 3, level: 'error'}], hasMore: false}))
  await errors
  assert.deepEqual(state.entries.map(entry => entry.id), [3])
  const pending = state.refresh()
  apps.at(-1).unmount()
  requests[4].resolve(response({entries: [{id: 4}], hasMore: false}))
  await pending
  assert.equal(requests[4].signal.aborted, true)
  assert.deepEqual(state.entries.map(entry => entry.id), [3])
})

test('request records render as focusable native buttons with selected state and working activation', async () => {
  window.setInterval = () => 1
  window.clearInterval = () => {}
  globalThis.fetch = async () => response({entries: [], total: 0})
  const state = mount('diagnostics/RequestDebug')
  await flush()
  state.entries = [{id: 1, timestamp: 'now', status: 200, tag: 'test', method: 'POST', url: 'https://example.com', requestBody: '{}', responseBody: '{}'}]
  const {descriptor} = parse(readFileSync(resolve(root, 'src/features/diagnostics/RequestDebug.vue'), 'utf8'))
  const template = compileTemplate({source: descriptor.template.content, filename: 'RequestDebug.vue', id: 'request-buttons', compilerOptions: {mode: 'function'}})
  assert.deepEqual(template.errors, [])
  const render = new Function('Vue', template.code)(vue)
  function findButton(node) {
    if (node?.props?.class?.includes('trace-item')) return node
    if (Array.isArray(node?.children)) {
      for (const child of node.children) {
        const result = findButton(child)
        if (result) return result
      }
    }
  }
  let tree
  const app = renderer.createApp({
    components: {RequestError: {render: () => null}},
    render() { tree = render(state, []); return vue.h('div') }
  })
  app.mount({})
  apps.push(app)
  let button = findButton(tree)
  assert.equal(button.type, 'button')
  assert.equal(button.props.type, 'button')
  assert.equal(button.props.tabindex, undefined)
  assert.equal(button.props['aria-pressed'], false)
  button.props.onClick()
  assert.equal(state.selected.id, 1)
  await vue.nextTick()
  button = findButton(tree)
  assert.equal(button.props['aria-pressed'], true)
})

test('clearing a session never clears another session and blocks duplicate requests', async () => {
  window.confirm = () => true
  const state = mount('conversation/GroupManager')
  await flush()
  state.selectedGroup = 'A'
  state.selectedGroupName = 'session A'
  state.chatRecords = [{id: 1, content: 'A'}]
  const pending = deferred()
  let deletes = 0
  globalThis.fetch = async (_url, options) => {
    if (options?.method === 'DELETE') { deletes++; return pending.promise }
    return response([])
  }
  const clearing = state.clearGroupRecords()
  await state.clearGroupRecords()
  assert.equal(deletes, 1)
  state.selectedGroup = 'B'
  state.chatRecords = [{id: 2, content: 'B'}]
  pending.resolve(response({success: true}))
  await clearing
  assert.equal(state.chatRecords[0].content, 'B')
  assert.equal(state.clearingSessions.size, 0)
})

test('emoji save completion cannot discard a newer editor or a draft changed during saving', async () => {
  window.confirm = () => true
  const state = mount('tools/EmojiManager')
  await flush()
  const a = {res_id: 'A', summary: 'original A'}
  const b = {res_id: 'B', summary: 'original B'}
  state.emojis = [a, b]
  state.startEdit(a)
  state.editingDesc = 'saved A'
  const pending = deferred()
  let posts = 0
  globalThis.fetch = async (_url, options) => {
    if (options?.method === 'POST') { posts++; return pending.promise }
    return response([a, b])
  }
  const saving = state.saveDesc(a)
  state.startEdit(b)
  state.editingDesc = 'draft B'
  await state.saveDesc(b)
  assert.equal(posts, 1)
  pending.resolve(response({success: true}))
  await saving
  assert.equal(state.editingId, 'B')
  assert.equal(state.editingDesc, 'draft B')
})

for (const [page, action, pendingKey, arg] of [
  ['access/AccessManager', 'removeAdmin', 'removingAdmins', '42'],
  ['access/AccessManager', 'removeBlacklistEntry', 'removingBlacklist', '42'],
  ['tools/CustomTools', 'deleteTool', 'pendingTools', 42],
  ['conversation/GroupManager', 'removeGroup', 'removingGroups', '42'],
  ['conversation/GroupManager', 'deleteRecord', 'deletingRecords', 42]
]) {
  test(`${page}/${action}: duplicate mutations are blocked and failures release pending state`, async () => {
    window.confirm = () => true
    const state = mount(page)
    await flush()
    const pending = deferred()
    let count = 0
    globalThis.fetch = () => { count++; return pending.promise }
    const first = state[action](arg)
    await state[action](arg)
    assert.equal(count, 1)
    pending.reject(new TypeError('offline'))
    await first
    assert.equal(state[pendingKey].size, 0)
    assert.equal(toasts.at(-1)[1], true)
  })
}

test('live log pushes obey committed filters and survive an in-flight snapshot without duplicates', async t => {
  const oldLocation = globalThis.location
  globalThis.location = {protocol: 'http:', host: 'localhost'}
  t.after(() => { if (oldLocation === undefined) delete globalThis.location; else globalThis.location = oldLocation })
  globalThis.WebSocket = class { static OPEN = 1; readyState = 1; send() {}; close() {} }
  window.requestAnimationFrame = () => 1
  window.cancelAnimationFrame = () => {}
  window.clearTimeout = () => {}
  globalThis.fetch = async url => response(url === '/admin/api/groups' ? [] : {entries: [], hasMore: false})
  const state = mount('diagnostics/LogViewer')
  await flush()
  const pending = deferred()
  globalThis.fetch = () => pending.promise
  state.level = 'error'
  const refresh = state.refresh()
  const entry = (id, level) => ({id, level, sessionId: '0', source: 'test', content: 'event'})
  const push = data => state.handleWsMessage({data: JSON.stringify({type: 'log', data})})
  push(entry(1, 'info'))
  push(entry(2, 'error'))
  push(entry(3, 'error'))
  pending.resolve(response({entries: [entry(2, 'error')], hasMore: false}))
  await refresh
  assert.deepEqual(state.entries.map(item => item.id), [2, 3])
  state.keyword = 'unsubmitted text'
  push(entry(4, 'error'))
  assert.equal(state.entries.at(-1).id, 4)
})

test('usage distinguishes initial failure from zero, supports retry and preserves successful data after refresh failure', async () => {
  globalThis.fetch = async () => { throw new TypeError('offline') }
  const state = mount('llm/UsageStats')
  assert.equal(state.loaded, false)
  await flush()
  assert.equal(state.loaded, false)
  assert.ok(state.loadError)
  globalThis.fetch = async () => response({total_calls: 4, total_tokens: 123, by_role: [], by_day: [], today: {calls: 4, total: 123}})
  await state.loadUsage()
  assert.equal(state.loaded, true)
  assert.equal(state.loadError, '')
  assert.equal(state.totalTokens, 123)
  globalThis.fetch = async () => ({ok: false, status: 503})
  await state.loadUsage()
  assert.equal(state.loaded, true)
  assert.equal(state.totalTokens, 123)
  assert.match(state.loadError, /503/)
})

test('dashboard sections fail independently, retry in place and keep previous values on failure', async () => {
  window.setInterval = () => 1
  window.clearInterval = () => {}
  globalThis.fetch = async url => {
    if (url === '/admin/api/groups') return response([{enabled: true, messageCount: 3}])
    if (url === '/admin/api/admins') throw new TypeError('offline')
    if (url === '/admin/api/llm-configs') return response({router: {model: 'fixture-model'}})
    return response([])
  }
  const state = mount('overview/Dashboard')
  await flush()
  assert.equal(state.dataState.groups.loaded, true)
  assert.equal(state.totalMessages, 3)
  assert.equal(state.dataState.admins.loaded, false)
  assert.ok(state.dataState.admins.error)
  assert.equal(state.dataState.models.loaded, true)
  globalThis.fetch = async () => response([{}, {}])
  await state.loadAdmins()
  assert.equal(state.dataState.admins.loaded, true)
  assert.equal(state.dataState.admins.error, '')
  assert.equal(state.adminCount, 2)
  globalThis.fetch = async () => ({...response({error: 'unavailable'}), ok: false, status: 503})
  await state.loadAdmins()
  assert.equal(state.adminCount, 2)
  assert.ok(state.dataState.admins.error)
  await state.loadLLMModels()
  assert.equal(state.llmModels.router, 'fixture-model')
})

test('log disconnect is visible, reconnect catches up and unmount disconnects the dedicated socket', async t => {
  const oldLocation = globalThis.location
  globalThis.location = {protocol: 'http:', host: 'localhost'}
  t.after(() => { if (oldLocation === undefined) delete globalThis.location; else globalThis.location = oldLocation })
  const sockets = []
  globalThis.WebSocket = class {
    static OPEN = 1
    readyState = 0
    constructor() { sockets.push(this) }
    send() {}
    close() { this.closed = true }
  }
  let reconnect, logQueries = 0
  window.requestAnimationFrame = () => 1
  window.cancelAnimationFrame = () => {}
  window.setTimeout = callback => { reconnect = callback; return 1 }
  window.clearTimeout = () => {}
  globalThis.fetch = async url => {
    if (url === '/admin/api/groups') return response([])
    logQueries++
    return response({entries: [], hasMore: false})
  }
  const state = mount('diagnostics/LogViewer')
  await flush()
  sockets[0].readyState = 1
  sockets[0].onopen()
  await flush()
  assert.equal(state.connectionState, 'connected')
  const beforeReconnect = logQueries
  sockets[0].onclose()
  assert.equal(state.connectionState, 'reconnecting')
  assert.match(state.connectionLabel, /数据可能过期/)
  reconnect()
  sockets[1].readyState = 1
  sockets[1].onopen()
  await flush()
  assert.equal(state.connectionState, 'connected')
  assert.equal(logQueries, beforeReconnect + 1)
  apps.at(-1).unmount()
  assert.equal(sockets[1].closed, true)
  assert.equal(sockets[1].onmessage, null)
})

test('request record clear prevents an older load from restoring deleted records and blocks duplicate clears', async () => {
  window.setInterval = () => 1
  window.clearInterval = () => {}
  window.confirm = () => true
  const initial = deferred(), deletion = deferred()
  let loads = 0, deletes = 0
  globalThis.fetch = async (_url, options) => {
    if (options?.method === 'DELETE') { deletes++; return deletion.promise }
    loads++
    return loads === 1 ? initial.promise : response({entries: [], total: 0})
  }
  const state = mount('diagnostics/RequestDebug')
  const clearing = state.clearAll()
  await state.clearAll()
  assert.equal(deletes, 1)
  initial.resolve(response({entries: [{id: 1}], total: 1}))
  await flush()
  assert.equal(state.entries.length, 0)
  deletion.resolve(response({success: true}))
  await clearing
  assert.equal(state.entries.length, 0)
  assert.equal(state.clearing, false)
})
