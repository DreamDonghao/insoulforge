import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {afterEach, beforeEach, test} from 'node:test'
import ts from 'typescript'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/composables/useLogoutConfirmation.ts', import.meta.url), 'utf8')
const code = ts.transpileModule(source, {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}}).outputText
const module = {exports: {}}
new Function('require', 'module', 'exports', code)(() => vue, module, module.exports)
const {useLogoutConfirmation} = module.exports
const renderer = vue.createRenderer({
  createElement: () => ({}), createText: () => ({}), createComment: () => ({}), insert() {}, remove() {},
  setText() {}, setElementText() {}, patchProp() {}, parentNode: () => null, nextSibling: () => null
})
let apps, originalFetch
beforeEach(() => { apps = []; originalFetch = globalThis.fetch })
afterEach(() => { apps.forEach(app => app.unmount()); globalThis.fetch = originalFetch })
function view() {
  const guards = new Set(), messages = [], session = {authenticated: true, disconnects: 0}
  let state
  const app = renderer.createApp({setup() {
    state = useLogoutConfirmation(guards, () => { session.authenticated = false; session.disconnects++ }, () => messages.push('busy'))
    return () => null
  }})
  app.mount({})
  apps.push(app)
  return {state, guards, messages, session, unmount: () => app.unmount()}
}
const success = () => ({ok: true, json: async () => ({success: true})})

test('opening and cancelling confirmation never send a request or discard drafts', () => {
  const v = view()
  let calls = 0
  globalThis.fetch = async () => { calls++; return success() }
  v.guards.add({dirty: () => true, busy: () => false})
  v.state.request()
  assert.equal(v.state.visible.value, true)
  assert.equal(v.state.hasUnsavedChanges(), true)
  v.state.cancel()
  assert.equal(v.state.visible.value, false)
  assert.equal(v.state.hasUnsavedChanges(), true)
  assert.equal(v.session.authenticated, true)
  assert.equal(calls, 0)
  v.guards.clear()
  assert.equal(v.state.hasUnsavedChanges(), false)
})

test('saving blocks both opening and confirming logout', async () => {
  const v = view()
  let busy = true, calls = 0
  v.guards.add({dirty: () => true, busy: () => busy})
  globalThis.fetch = async () => { calls++; return success() }
  v.state.request()
  assert.equal(v.state.visible.value, false)
  busy = false
  v.state.request()
  busy = true
  await v.state.confirm()
  assert.equal(v.state.visible.value, true)
  assert.equal(calls, 0)
  assert.equal(v.messages.length, 2)
})

test('only explicit confirmation sends one request and success ends the session', async () => {
  const v = view()
  let calls = 0, complete
  globalThis.fetch = (url, options) => {
    assert.equal(url, '/admin/api/auth/logout')
    assert.equal(options.method, 'POST')
    calls++
    return new Promise(resolve => { complete = resolve })
  }
  await v.state.confirm()
  assert.equal(calls, 0)
  v.state.request()
  const pending = v.state.confirm()
  await v.state.confirm()
  v.state.cancel()
  assert.equal(v.state.submitting.value, true)
  assert.equal(v.state.visible.value, true)
  assert.equal(v.session.authenticated, true)
  complete(success())
  await pending
  assert.equal(calls, 1)
  assert.equal(v.session.authenticated, false)
  assert.equal(v.session.disconnects, 1)
  assert.equal(v.state.visible.value, false)
})

test('failed responses preserve the session and draft, and allow an explicit retry', async () => {
  for (const response of [() => Promise.reject(Error('offline')), () => ({ok: false}),
    () => ({ok: true, json: async () => ({success: false})})]) {
    const v = view()
    v.guards.add({dirty: () => true, busy: () => false})
    globalThis.fetch = async () => response()
    v.state.request()
    await v.state.confirm()
    assert.equal(v.state.visible.value, true)
    assert.equal(v.state.submitting.value, false)
    assert.ok(v.state.error.value)
    assert.equal(v.session.authenticated, true)
    assert.equal(v.state.hasUnsavedChanges(), true)
    globalThis.fetch = async () => success()
    await v.state.confirm()
    assert.equal(v.session.authenticated, false)
  }
})

test('a stalled request times out and releases the confirmation controls', async t => {
  t.mock.timers.enable({apis: ['setTimeout']})
  const v = view()
  globalThis.fetch = (_url, {signal}) => new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')))
  })
  v.state.request()
  const pending = v.state.confirm()
  t.mock.timers.tick(10000)
  await pending
  assert.equal(v.state.submitting.value, false)
  assert.equal(v.state.visible.value, true)
  assert.equal(v.session.authenticated, true)
  assert.ok(v.state.error.value)
})

test('unmount aborts the request without logging out or showing a late error', async () => {
  const v = view()
  let signal
  globalThis.fetch = (_url, options) => new Promise((_resolve, reject) => {
    signal = options.signal
    signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')))
  })
  v.state.request()
  const pending = v.state.confirm()
  v.unmount()
  await pending
  assert.equal(signal.aborted, true)
  assert.equal(v.session.authenticated, true)
  assert.equal(v.state.error.value, '')
})
