import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {afterEach, beforeEach, test} from 'node:test'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

const require = createRequire(import.meta.url)
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const modules = new Map()

// Execute the actual setup scripts with Vue's lifecycle, without a browser DOM.
function loadModule(path) {
  if (modules.has(path)) return modules.get(path).exports
  const module = {exports: {}}
  modules.set(path, module)
  const source = readFileSync(path, 'utf8')
  const script = path.endsWith('.vue')
    ? compileScript(parse(source).descriptor, {id: path}).content : source
  const {outputText} = ts.transpileModule(script, {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
  })
  new Function('require', 'module', 'exports', outputText)(specifier => {
    if (specifier === 'vue') return vue
    if (!specifier.startsWith('.')) return require(specifier)
    if (specifier.endsWith('.vue')) return {default: {render: () => null}}
    return loadModule(resolve(dirname(path), `${specifier}.ts`))
  }, module, module.exports)
  if (path.endsWith('.vue')) module.exports.default.render = () => null
  return module.exports
}

const renderer = vue.createRenderer({
  createElement: () => ({}), createText: () => ({}), createComment: () => ({}),
  insert() {}, remove() {}, setText() {}, setElementText() {}, patchProp() {},
  parentNode: () => null, nextSibling: () => null
})

class Socket extends EventTarget {
  static OPEN = 1
  static CLOSED = 3
  readyState = Socket.OPEN
  listeners = new Set()
  sent = []
  onmessage = null
  addEventListener(type, handler) {
    super.addEventListener(type, handler)
    if (type === 'message') this.listeners.add(handler)
  }
  removeEventListener(type, handler) {
    super.removeEventListener(type, handler)
    if (type === 'message') this.listeners.delete(handler)
  }
  send(text) {
    assert.equal(this.readyState, Socket.OPEN)
    this.sent.push(JSON.parse(text))
  }
  emit(data) {
    const event = new MessageEvent('message', {data: JSON.stringify(data)})
    this.onmessage?.(event)
    this.dispatchEvent(event)
  }
}

let socket, connected, apps, calls, toasts, restoreGlobals
const flush = async () => { await vue.nextTick(); await new Promise(resolve => setImmediate(resolve)) }
const response = data => ({ok: true, json: async () => data})

beforeEach(() => {
  socket = new Socket()
  connected = vue.ref(true)
  apps = new Set()
  calls = []
  toasts = []
  const originals = Object.fromEntries(['WebSocket', 'window', 'document', 'fetch'].map(key => [key, globalThis[key]]))
  restoreGlobals = () => {
    for (const [key, value] of Object.entries(originals)) {
      if (value === undefined) delete globalThis[key]
      else globalThis[key] = value
    }
  }
  globalThis.WebSocket = Socket
  globalThis.window = {setInterval: () => 1, clearInterval() {}}
  globalThis.document = {body: {style: {overflow: ''}}}
  globalThis.fetch = async path => {
    calls.push(path)
    return response(path === '/admin/api/groups' || path.includes('/chat-records/') ? [] : {})
  }
})

afterEach(() => {
  for (const app of apps) app.unmount()
  restoreGlobals()
})

function mount(component) {
  const app = renderer.createApp(component)
  app.provide('ws', {get: () => socket})
  app.provide('wsConnected', connected)
  app.provide('qqConfig', {botName: 'Bot'})
  app.provide('showToast', (...args) => toasts.push(args))
  app.mount({})
  apps.add(app)
  return {state: app._instance.setupState, unmount() { app.unmount(); apps.delete(app) }}
}

const useAdminWebSocket = loadModule(resolve(root, 'src/composables/useAdminWebSocket.ts')).useAdminWebSocket
const useCoalescedRefresh = loadModule(resolve(root, 'src/composables/useCoalescedRefresh.ts')).useCoalescedRefresh

function controlUsageRequests() {
  const originalFetch = globalThis.fetch
  const requests = []
  let active = 0, peak = 0
  globalThis.fetch = (path, options) => {
    if (!path.startsWith('/admin/api/usage?')) return originalFetch(path, options)
    active++
    peak = Math.max(peak, active)
    return new Promise((resolve, reject) => requests.push({
      path, signal: options.signal,
      respond(value) { active--; resolve(value) },
      complete(data) { this.respond(response(data)) },
      fail(error = new Error('network failed')) { active--; reject(error) }
    }))
  }
  return {requests, peak: () => peak}
}

test('a notification at the end of a refresh cycle is not lost', async () => {
  let finish, count = 0
  const view = mount({
    setup() {
      return useCoalescedRefresh(() => {
        count++
        return new Promise(resolve => { finish = resolve })
      })
    },
    render: () => null
  })
  const first = view.state.refresh()
  finish()
  let second
  queueMicrotask(() => { second = view.state.refresh() })
  await first
  assert.equal(count, 2)
  assert.equal(view.state.loading, true)
  finish()
  await second
  assert.equal(view.state.loading, false)
})

test('listeners coexist, rebind on reconnect, and clean up the original socket', () => {
  let first = 0, second = 0, existing = 0
  const original = socket
  const existingHandler = () => ++existing
  original.onmessage = existingHandler
  const one = mount({setup() { useAdminWebSocket(() => ++first); return () => null }})
  const two = mount({setup() { useAdminWebSocket(() => ++second); return () => null }})
  assert.equal(original.listeners.size, 2)
  original.emit({type: 'test'})
  assert.deepEqual([first, second, existing], [1, 1, 1])
  assert.equal(original.onmessage, existingHandler)

  connected.value = false
  assert.equal(original.listeners.size, 0)
  socket = new Socket()
  connected.value = true
  connected.value = true
  assert.equal(socket.listeners.size, 2)
  socket.emit({type: 'test'})
  assert.deepEqual([first, second], [2, 2])
  one.unmount()
  socket.emit({type: 'test'})
  assert.deepEqual([first, second], [2, 3])

  const bound = socket
  socket = new Socket()
  two.unmount()
  assert.equal(bound.listeners.size, 0)
  connected.value = false
  connected.value = true
  assert.equal(socket.listeners.size, 0)
})

test('connecting and closed sockets are never used for sending', () => {
  socket.readyState = 0
  connected.value = false
  let send
  mount({setup() { ({send} = useAdminWebSocket(() => {})); return () => null }})
  send({action: 'test'})
  assert.equal(socket.sent.length, 0)
  socket.readyState = Socket.OPEN
  connected.value = true
  send({action: 'test'})
  assert.equal(socket.sent.length, 1)
  socket.readyState = Socket.CLOSED
  send({action: 'test'})
  assert.equal(socket.sent.length, 1)
})

for (const page of ['overview/Dashboard', 'llm/UsageStats']) {
  test(`${page}: repeated visits and reconnects process each usage event once`, async () => {
    const component = loadModule(resolve(root, `src/features/${page}.vue`)).default
    const count = () => calls.filter(path => path.startsWith('/admin/api/usage?')).length
    for (let i = 0; i < 3; i++) {
      const view = mount(component)
      await flush()
      const before = count()
      socket.emit({type: 'usage_updated'})
      await flush()
      assert.equal(count(), before + 1)
      const previous = socket
      connected.value = false
      previous.readyState = Socket.CLOSED
      socket = new Socket()
      connected.value = true
      previous.emit({type: 'usage_updated'})
      socket.emit({type: 'usage_updated'})
      await flush()
      assert.equal(count(), before + 2)
      view.unmount()
      assert.equal(socket.listeners.size, 0)
      socket.emit({type: 'usage_updated'})
      await flush()
      assert.equal(count(), before + 2)
    }
  })

  test(`${page}: initial, manual and pushed refreshes coalesce without dropping the latest data`, async () => {
    const controlled = controlUsageRequests()
    const view = mount(loadModule(resolve(root, `src/features/${page}.vue`)).default)
    const {requests} = controlled
    assert.equal(requests.length, 1)
    assert.equal(requests[0].path, `/admin/api/usage?days=${page === 'overview/Dashboard' ? 7 : 30}`)
    const completion = view.state.loadUsage()
    assert.equal(view.state.loadUsage(), completion)
    for (let i = 0; i < 20; i++) socket.emit({type: 'usage_updated'})
    assert.equal(requests.length, 1)
    requests[0].complete({today: {calls: 1}})
    await flush()
    assert.equal(requests.length, 2)
    if (page === 'llm/UsageStats') assert.equal(view.state.loading, true)
    assert.equal(view.state.todayStats.calls, 1)
    for (let i = 0; i < 20; i++) socket.emit({type: 'usage_updated'})
    requests[1].complete({today: {calls: 2}})
    await flush()
    assert.equal(requests.length, 3)
    requests[2].complete({today: {calls: 3}})
    await completion
    assert.equal(view.state.todayStats.calls, 3)
    assert.equal(controlled.peak(), 1)
    assert.equal(requests.length, 3)
    if (page === 'llm/UsageStats') assert.equal(view.state.loading, false)
  })

  test(`${page}: failed requests preserve queued notifications but do not retry on their own`, async () => {
    const {requests} = controlUsageRequests()
    const view = mount(loadModule(resolve(root, `src/features/${page}.vue`)).default)
    socket.emit({type: 'usage_updated'})
    requests[0].fail()
    await flush()
    assert.equal(requests.length, 2)
    requests[1].complete({today: {calls: 2}})
    await flush()
    assert.equal(view.state.todayStats.calls, 2)
    const failed = view.state.loadUsage()
    requests[2].fail()
    await failed
    await flush()
    assert.equal(requests.length, 3)
    const recovered = view.state.loadUsage()
    requests[3].complete({today: {calls: 4}})
    await recovered
    assert.equal(view.state.todayStats.calls, 4)
  })

  test(`${page}: unmount aborts a pending request without follow-up or error notification`, async () => {
    const {requests} = controlUsageRequests()
    const view = mount(loadModule(resolve(root, `src/features/${page}.vue`)).default)
    const completion = view.state.loadUsage()
    socket.emit({type: 'usage_updated'})
    view.unmount()
    assert.equal(requests[0].signal.aborted, true)
    requests[0].fail()
    await completion
    assert.equal(requests.length, 1)
    assert.deepEqual(toasts, [])
    assert.equal(view.state.todayStats.calls, 0)
    await view.state.loadUsage()
    assert.equal(requests.length, 1)
  })

  test(`${page}: response bodies completed after unmount cannot update the page`, async () => {
    const {requests} = controlUsageRequests()
    const view = mount(loadModule(resolve(root, `src/features/${page}.vue`)).default)
    let finishBody
    requests[0].respond({ok: true, json: () => new Promise(resolve => { finishBody = resolve })})
    await flush()
    const completion = view.state.loadUsage()
    assert.equal(requests.length, 1)
    view.unmount()
    finishBody({today: {calls: 999}})
    await completion
    assert.equal(view.state.todayStats.calls, 0)
    assert.equal(requests.length, 1)
    assert.deepEqual(toasts, [])
  })
}

test('usage HTTP errors keep their existing message and allow the queued refresh', async () => {
  const {requests} = controlUsageRequests()
  const view = mount(loadModule(resolve(root, 'src/features/llm/UsageStats.vue')).default)
  const completion = view.state.loadUsage()
  requests[0].respond({ok: false, status: 503})
  await flush()
  assert.deepEqual(toasts, [['加载失败: 503', true]])
  assert.equal(requests.length, 2)
  assert.equal(view.state.loading, true)
  requests[1].complete({today: {calls: 1}})
  await completion
  assert.equal(view.state.loading, false)
  assert.equal(view.state.todayStats.calls, 1)
})

const groupComponent = () => loadModule(resolve(root, 'src/features/conversation/GroupManager.vue')).default

test('repeated group page visits do not retain old message handlers', async () => {
  for (let i = 0; i < 3; i++) {
    const view = mount(groupComponent())
    await flush()
    await view.state.selectGroup('123', 'Test')
    socket.emit({type: 'new_message', groupId: '123', data: {role: 'user', content: 'test'}})
    assert.equal(view.state.chatRecords.length, 1)
    view.unmount()
    assert.equal(socket.listeners.size, 0)
    socket.emit({type: 'new_message', groupId: '123', data: {role: 'user', content: 'late'}})
    assert.equal(view.state.chatRecords.length, 1)
  }
})

test('group subscriptions resume after reconnect and stop on return or unmount', async () => {
  const view = mount(groupComponent())
  await flush()
  assert.equal(socket.listeners.size, 1)
  assert.deepEqual(socket.sent, [])
  await view.state.selectGroup('123', 'Test')
  assert.deepEqual(socket.sent, [{action: 'subscribe', groupId: '123'}])
  const event = {type: 'new_message', groupId: '123', data: {role: 'user', content: 'test'}}
  socket.emit(event)
  assert.equal(view.state.chatRecords.length, 1)
  const previous = socket
  connected.value = false
  previous.readyState = Socket.CLOSED
  socket = new Socket()
  connected.value = true
  assert.deepEqual(socket.sent, [{action: 'subscribe', groupId: '123'}])
  previous.emit(event)
  socket.emit(event)
  assert.equal(view.state.chatRecords.length, 2)
  view.state.backToList()
  assert.deepEqual(socket.sent.at(-1), {action: 'unsubscribe'})
  connected.value = false
  socket = new Socket()
  connected.value = true
  assert.deepEqual(socket.sent, [])
  await view.state.selectGroup('456', 'Next')
  view.unmount()
  assert.deepEqual(socket.sent.at(-1), {action: 'unsubscribe'})
  assert.equal(socket.listeners.size, 0)
})

test('group loading completed after unmount never attaches a listener', async () => {
  let finish
  globalThis.fetch = () => new Promise(resolve => { finish = resolve })
  const view = mount(groupComponent())
  view.unmount()
  finish(response([]))
  await flush()
  assert.equal(socket.listeners.size, 0)
  assert.deepEqual(socket.sent, [])
})

test('late chat snapshots never resubscribe after leaving the page or list', async () => {
  for (const leavePage of [false, true]) {
    let finish
    const view = mount(groupComponent())
    await flush()
    globalThis.fetch = () => new Promise(resolve => { finish = resolve })
    const pending = view.state.selectGroup('123', 'Test')
    if (leavePage) view.unmount()
    else view.state.backToList()
    socket.sent = []
    finish(response([{id: 1, role: 'user', content: 'late'}]))
    await pending
    assert.deepEqual(socket.sent, [])
    assert.equal(view.state.chatRecords.length, 0)
    if (!leavePage) view.unmount()
    globalThis.fetch = async () => response([])
  }
})
