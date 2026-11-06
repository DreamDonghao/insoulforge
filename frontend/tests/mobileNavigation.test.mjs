import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import ts from 'typescript'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/composables/useMobileNavigation.ts', import.meta.url), 'utf8')
const code = ts.transpileModule(source, {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}}).outputText
const module = {exports: {}}
new Function('require', 'module', 'exports', code)(() => vue, module, module.exports)
const renderer = vue.createRenderer({
  createComment: () => ({}), insert() {}, remove() {}, parentNode: () => null, nextSibling: () => null
})

test('mobile navigation starts closed, follows the breakpoint and releases its listener', () => {
  const original = globalThis.window
  const listeners = new Set()
  const media = {matches: true, addEventListener: (_, fn) => listeners.add(fn), removeEventListener: (_, fn) => listeners.delete(fn)}
  globalThis.window = {matchMedia: query => { assert.equal(query, '(max-width: 760px)'); return media }}
  let state
  const app = renderer.createApp({setup() { state = module.exports.useMobileNavigation(); return () => null }})
  try {
    app.mount({})
    assert.equal(state.isMobile.value, true)
    assert.equal(state.mobileNavigationOpen.value, false)
    state.mobileNavigationOpen.value = true
    media.matches = false
    listeners.forEach(fn => fn())
    assert.equal(state.isMobile.value, false)
    assert.equal(state.mobileNavigationOpen.value, false)
    media.matches = true
    listeners.forEach(fn => fn())
    assert.equal(state.isMobile.value, true)
    assert.equal(state.mobileNavigationOpen.value, false)
  } finally {
    app.unmount()
    assert.equal(listeners.size, 0)
    globalThis.window = original
  }
})

test('viewport changes wait until dirty or busy work can safely switch layouts', () => {
  const original = globalThis.window
  let change
  const media = {matches:false,addEventListener:(_,fn)=>{change=fn},removeEventListener(){}}
  globalThis.window={matchMedia:()=>media}
  const dirty=vue.ref(true),busy=vue.ref(false)
  let state
  const app=renderer.createApp({setup(){state=module.exports.useMobileNavigation(()=>!dirty.value&&!busy.value);return()=>null}})
  try {
    app.mount({})
    media.matches=true;change()
    assert.equal(state.isMobile.value,false)
    busy.value=true;dirty.value=false
    assert.equal(state.isMobile.value,false)
    busy.value=false
    assert.equal(state.isMobile.value,true)
  } finally {app.unmount();globalThis.window=original}
})
