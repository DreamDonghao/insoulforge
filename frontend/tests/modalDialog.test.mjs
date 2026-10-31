import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {beforeEach, afterEach, test} from 'node:test'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/components/ModalDialog.vue', import.meta.url), 'utf8')
const script = compileScript(parse(source).descriptor, {id: 'modal-test', inlineTemplate: true}).content
const {outputText} = ts.transpileModule(script, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
})
const module = {exports: {}}
const toastModule = {exports: {}}
const toastCode = ts.transpileModule(readFileSync(new URL('../src/composables/useToast.ts', import.meta.url), 'utf8'), {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
}).outputText
new Function('require', 'module', 'exports', toastCode)(() => vue, toastModule, toastModule.exports)
new Function('require', 'module', 'exports', outputText)(name => name === 'vue' ? vue : toastModule.exports, module, module.exports)
let apps, nodes, originalDocument, originalHTMLElement
class Control {
  tabIndex = 0
  disabled = false
  visible = true
  isConnected = true
  focus() { document.activeElement = this }
  matches() { return this.disabled }
  closest() { return null }
  getClientRects() { return this.visible ? [{}] : [] }
}
const renderer = vue.createRenderer({
  createElement() {
    const node = new Control()
    Object.assign(node, {
      props: {}, controls: [], initial: null, opened: false,
      showModal() { this.opened = true }, close() { this.opened = false },
      querySelector() { return this.initial }, querySelectorAll() { return this.controls },
      contains(target) { return target === this || this.controls.includes(target) }
    })
    nodes.push(node)
    return vue.markRaw(node)
  },
  createText: () => ({}), createComment: () => ({}), insert() {}, remove() {}, setText() {}, setElementText() {},
  patchProp(node, key, _old, value) { node.props[key] = value },
  parentNode: () => null, nextSibling: () => null
})
beforeEach(() => {
  apps = []
  nodes = []
  originalDocument = globalThis.document
  originalHTMLElement = globalThis.HTMLElement
  globalThis.HTMLElement = Control
  globalThis.document = {body: {style: {overflow: 'auto'}}, activeElement: new Control(), querySelector: () => null}
})
afterEach(() => {
  apps.reverse().forEach(app => app.unmount())
  if (originalDocument === undefined) delete globalThis.document
  else globalThis.document = originalDocument
  if (originalHTMLElement === undefined) delete globalThis.HTMLElement
  else globalThis.HTMLElement = originalHTMLElement
})
function mount(close = () => {}) {
  const app = renderer.createApp(module.exports.default, {labelledby: 'example-title', onClose: close})
  app.mount({})
  apps.push(app)
  return nodes.at(-1)
}
function key(node, shiftKey = false) {
  const event = {key: 'Tab', shiftKey, prevented: false, preventDefault() { this.prevented = true }}
  node.props.onKeydown(event)
  return event
}

test('dialog opens modally with a title, locks scrolling, then restores focus and previous scrolling', () => {
  const trigger = document.activeElement
  const dialog = mount()
  assert.equal(dialog.opened, true)
  assert.equal(toastModule.exports.useToast().toastTarget.value, dialog)
  assert.equal(dialog.props['aria-labelledby'], 'example-title')
  assert.equal(document.body.style.overflow, 'hidden')
  new Control().focus()
  apps.pop().unmount()
  assert.equal(dialog.opened, false)
  assert.equal(toastModule.exports.useToast().toastTarget.value, null)
  assert.equal(document.activeElement, trigger)
  assert.equal(document.body.style.overflow, 'auto')
})

test('Tab wraps in both directions and ignores disabled and invisible controls', () => {
  const dialog = mount()
  const first = new Control(), last = new Control(), disabled = new Control(), hidden = new Control()
  disabled.disabled = true
  hidden.visible = false
  dialog.controls = [hidden, first, disabled, last, hidden]
  first.focus()
  assert.equal(key(dialog, true).prevented, true)
  assert.equal(document.activeElement, last)
  assert.equal(key(dialog).prevented, true)
  assert.equal(document.activeElement, first)
  assert.equal(key(dialog).prevented, false)
})

test('a dialog with no enabled controls retains focus; Escape requests closure without bypassing the parent', () => {
  let requested = 0
  const dialog = mount(() => requested++)
  assert.equal(key(dialog).prevented, true)
  assert.equal(document.activeElement, dialog)
  const event = {prevented: false, preventDefault() { this.prevented = true }}
  dialog.props.onCancel(event)
  assert.equal(event.prevented, true)
  assert.equal(requested, 1)
  assert.equal(dialog.opened, true)
})

test('scrolling remains locked until the final modal unmounts', () => {
  mount()
  mount()
  apps.pop().unmount()
  assert.equal(document.body.style.overflow, 'hidden')
  apps.pop().unmount()
  assert.equal(document.body.style.overflow, 'auto')
})
