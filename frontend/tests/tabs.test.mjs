import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileScript, compileTemplate, parse} from 'vue/compiler-sfc'
import {renderToString} from 'vue/server-renderer'
import ts from 'typescript'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/components/SlidingTabs.vue', import.meta.url), 'utf8')
const {descriptor} = parse(source)
const script = compileScript(descriptor, {id: 'tabs-test'}).content
const {outputText} = ts.transpileModule(script, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
})
const module = {exports: {}}
new Function('require', 'module', 'exports', outputText)(() => vue, module, module.exports)
const template = compileTemplate({source: descriptor.template.content, filename: 'SlidingTabs.vue', id: 'tabs-test', compilerOptions: {mode: 'function'}})
assert.deepEqual(template.errors, [])
const render = new Function('Vue', template.code)(vue)
function tabs(accept = true, idPrefix = 'example') {
  const props = vue.reactive({modelValue: 'first', label: '配置类别', idPrefix, itemMinWidth: 104,
    items: [{key: 'first', label: '第一项'}, {key: 'second', label: '第二项'}, {key: 'third', label: '第三项'}]})
  const emitted = [], focused = [], scrolled = []
  const scope = vue.effectScope()
  const state = vue.proxyRefs(scope.run(() => module.exports.default.setup(props, {
    expose() {}, emit(name, value) { emitted.push([name, value]); if (accept) props.modelValue = value }
  })))
  state.tablist = {querySelector: selector => ({focus: () => focused.push(selector), scrollIntoView: () => scrolled.push(selector)})}
  return {state, props, emitted, focused, scrolled, dispose: () => scope.stop(),
    html: () => renderToString(vue.createSSRApp({render: () => vue.h({setup: () => ({...state, ...props}), render}, {},
      {panel: ({item}) => vue.h('p', `Panel ${item.key}`)})}))}
}
function press(view, key, item) {
  const event = {key, prevented: false, preventDefault() { this.prevented = true }}
  view.state.moveFocus(event, item)
  return event
}

test('arrows, Home and End move focus and wrap without changing selection or emitting updates', () => {
  const view = tabs()
  try {
    assert.equal(press(view, 'ArrowLeft', 'first').prevented, true)
    assert.equal(view.state.focusedKey, 'third')
    press(view, 'ArrowRight', 'third')
    assert.equal(view.state.focusedKey, 'first')
    press(view, 'End', 'first')
    assert.equal(view.state.focusedKey, 'third')
    press(view, 'Home', 'third')
    assert.equal(view.state.focusedKey, 'first')
    assert.equal(press(view, 'Tab', 'first').prevented, false)
    assert.equal(view.props.modelValue, 'first')
    assert.deepEqual(view.emitted, [])
    assert.equal(view.scrolled.length, 4)
  } finally { view.dispose() }
})

test('activating a tab selects its panel; a rejected switch restores focus to the accepted tab', async () => {
  for (const accepted of [true, false]) {
    const view = tabs(accepted)
    try {
      await view.state.activate('second')
      assert.equal(view.props.modelValue, accepted ? 'second' : 'first')
      assert.equal(view.state.focusedKey, view.props.modelValue)
      assert.ok(view.focused.at(-1).includes(view.state.tabId(view.props.modelValue)))
      assert.deepEqual(view.emitted, [['update:modelValue', 'second']])
    } finally { view.dispose() }
  }
})

test('all tabs have matching labelled panels; only the selected panel mounts its content', async () => {
  const view = tabs(), other = tabs(true, 'other')
  try {
    let html = await view.html()
    assert.equal((html.match(/role="tab"/g) || []).length, 3)
    assert.equal((html.match(/role="tabpanel"/g) || []).length, 3)
    assert.equal((html.match(/aria-selected="true"/g) || []).length, 1)
    assert.equal((html.match(/tabindex="-1"/g) || []).length, 2)
    assert.ok(html.includes('aria-controls="example-panel-first"'))
    assert.ok(html.includes('aria-labelledby="example-tab-first"'))
    assert.ok(html.includes('Panel first'))
    assert.equal(html.includes('Panel second'), false)
    await view.state.activate('second')
    html = await view.html()
    assert.ok(html.includes('Panel second'))
    assert.equal(html.includes('Panel first'), false)
    assert.notEqual(view.state.tabId('first'), other.state.tabId('first'))
  } finally { view.dispose(); other.dispose() }
})
