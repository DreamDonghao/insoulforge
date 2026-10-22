import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/features/conversation/ChatMessageView.vue', import.meta.url), 'utf8')
const script = compileScript(parse(source).descriptor, {id: 'chat-test', inlineTemplate: true}).content
const {outputText} = ts.transpileModule(script, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
})

const node = (tag, text = '') => ({tag, text, props: {}, children: [], parent: null})
const remove = child => {
  if (child.parent) child.parent.children.splice(child.parent.children.indexOf(child), 1)
  child.parent = null
}
const renderer = vue.createRenderer({
  createElement: node, createText: text => node('#text', text), createComment: text => node('#comment', text),
  insert(child, parent, anchor = null) {
    remove(child)
    const index = anchor ? parent.children.indexOf(anchor) : parent.children.length
    parent.children.splice(index, 0, child)
    child.parent = parent
  },
  remove,
  setText: (node, text) => { node.text = text },
  setElementText: (node, text) => { node.text = text; node.children = [] },
  patchProp: (node, key, _, value) => { node.props[key] = value },
  parentNode: node => node.parent,
  nextSibling: node => node.parent?.children[node.parent.children.indexOf(node) + 1] ?? null
})

function mount(messages) {
  const counts = {parse: 0, format: 0}
  const module = {exports: {}}
  new Function('require', 'module', 'exports', 'JSON', outputText)(() => vue, module, module.exports, {
    parse(text) { counts.parse++; return JSON.parse(text) },
    stringify(...args) { counts.format++; return JSON.stringify(...args) }
  })
  const state = vue.reactive({messages, botName: 'Bot', tick: 0})
  const events = []
  const root = node('root')
  const app = renderer.createApp({
    setup: () => () => vue.h('div', {'data-tick': state.tick}, state.messages.map(message =>
      vue.h('div', {key: message.id, class: 'chat-message'}, [vue.h(module.exports.default, {
        message, botName: state.botName,
        onEdit: () => events.push(['edit', message.id]), onDelete: () => events.push(['delete', message.id])
      })])))
  })
  app.mount(root)
  return {state, counts, events, root, unmount: () => app.unmount()}
}

const find = (node, predicate) => predicate(node) ? node : node.children.map(child => find(child, predicate)).find(Boolean)
const byClass = (root, name) => find(root, node => node.props.class?.split(' ').includes(name))
const byTag = (root, tag) => find(root, node => node.tag === tag)
const textOf = node => node ? node.text + node.children.filter(child => child.tag !== '#comment').map(textOf).join('') : ''
const toggle = (view, open) => byTag(view.root, 'details').props.onToggle({target: {open}})
const message = content => ({id: 1, role: 'user', content: typeof content === 'string' ? content : JSON.stringify(content)})

test('each message parses once; unrelated renders and appends reuse existing results', async () => {
  const view = mount([message({sender: {name: 'Alice'}, segments: [{type: 'text', text: 'Hello'}]})])
  try {
    assert.equal(view.counts.parse, 1)
    assert.equal(view.counts.format, 0)
    assert.equal(textOf(byClass(view.root, 'msg-role')), 'Alice')
    assert.equal(textOf(byClass(view.root, 'msg-content')), 'Hello')
    view.state.tick++
    await vue.nextTick()
    assert.equal(view.counts.parse, 1)
    view.state.messages.push({...message('second'), id: 2})
    await vue.nextTick()
    assert.equal(view.counts.parse, 2)
    view.state.messages[0].content = JSON.stringify({segments: [{type: 'text', text: 'Changed'}]})
    await vue.nextTick()
    assert.equal(view.counts.parse, 3)
    assert.equal(textOf(byClass(view.root, 'msg-content')), 'Changed')
    assert.equal(view.counts.format, 0)
  } finally { view.unmount() }
})

test('JSON is formatted lazily, cached across toggles, and invalidated by changed content', async () => {
  const first = {segments: [{type: 'text', text: 'Hello'}], extra: 'x'.repeat(10000)}
  const view = mount([message(first)])
  try {
    assert.equal(byTag(view.root, 'pre'), undefined)
    assert.equal(view.counts.format, 0)
    toggle(view, true)
    await vue.nextTick()
    assert.equal(textOf(byTag(view.root, 'pre')), JSON.stringify(first, null, 2))
    assert.equal(view.counts.format, 1)
    toggle(view, false)
    await vue.nextTick()
    toggle(view, true)
    await vue.nextTick()
    assert.equal(view.counts.format, 1)
    const changed = {text: 'Updated', extra: 'complete'}
    view.state.messages[0].content = JSON.stringify(changed)
    await vue.nextTick()
    assert.equal(view.counts.parse, 2)
    assert.equal(view.counts.format, 2)
    assert.equal(textOf(byTag(view.root, 'pre')), JSON.stringify(changed, null, 2))
    view.state.messages[0].content = 'plain'
    await vue.nextTick()
    assert.equal(byTag(view.root, 'details'), undefined)
    view.state.messages[0].content = JSON.stringify(first)
    await vue.nextTick()
    assert.equal(byTag(view.root, 'pre'), undefined)
    assert.equal(view.counts.format, 2)
  } finally { view.unmount() }
})

test('plain text, invalid JSON, and non-object JSON keep their original display', () => {
  for (const content of ['plain', '{broken', 'null', '[]', '42', 'true', '"text"']) {
    const view = mount([message(content)])
    try {
      assert.equal(textOf(byClass(view.root, 'msg-content')), content)
      assert.equal(byTag(view.root, 'details'), undefined)
      assert.equal(view.counts.parse, 1)
      assert.equal(view.counts.format, 0)
    } finally { view.unmount() }
  }
})

test('message segments and old image descriptions retain their existing rendering', () => {
  const cases = [
    [{}, '[空消息]'], [{text: 'fallback'}, 'fallback'],
    [{segments: [null, 'invalid', {type: 'text', text: 'text'}]}, 'text'],
    [{segments: [{type: 'at', target: {kind: 'all'}}]}, '@全体成员'],
    [{segments: [{type: 'at', qq: '0'}]}, '@全体成员'],
    [{segments: [{type: 'at', target: {name: 'Alice', qq: '123'}}]}, '@Alice'],
    [{segments: [{type: 'at', name: 'Legacy'}]}, '@Legacy'],
    [{segments: [{type: 'at', qq: '123'}]}, '@123'],
    [{segments: [{type: 'at'}]}, '@成员'],
    [{segments: [{type: 'image', description: 'direct'}]}, '[图片] direct'],
    [{segments: [{type: 'image', image_index: 0}], assets: {images: [{description: 'asset'}]}}, '[图片] asset'],
    [{segments: [{type: 'image', image_index: 0}], images: [{description: 'old'}]}, '[图片] old'],
    [{segments: [{type: 'image', image_index: 9}]}, '[图片]'],
    [{segments: [{type: 'face', label: '笑'}]}, '[QQ 表情：笑]'],
    [{segments: [{type: 'face'}]}, '[QQ 表情]'],
    [{segments: [{type: 'sticker', name: 'Cat'}]}, '[表情包：Cat]'],
    [{segments: [{type: 'sticker'}]}, '[表情包：未命名]'],
    [{segments: [{type: 'poke', target: {name: 'Alice'}}]}, '[拍一拍：Alice]'],
    [{segments: [{type: 'poke', target_name: 'Old'}]}, '[拍一拍：Old]'],
    [{segments: [{type: 'poke'}]}, '[拍一拍]'],
    [{segments: [{type: 'notification', action: 'join'}]}, '[成员事件：join]'],
    [{segments: [{type: 'member_event', kind: 'leave'}]}, '[成员事件：leave]'],
    [{segments: [{type: 'notification'}]}, '[成员事件]'],
    [{segments: [{type: 'unsupported', segment_type: 'video'}]}, '[video]'],
    [{segments: [{type: 'unknown'}]}, '[unknown]'],
    [{segments: [{}]}, '[未知消息]']
  ]
  for (const [content, expected] of cases) {
    const view = mount([message(content)])
    try {
      assert.equal(textOf(byClass(view.root, 'msg-content')), expected)
      assert.equal(view.counts.parse, 1)
    } finally { view.unmount() }
  }
})

test('sender fallback reacts to role and bot name without reparsing; edit/delete events are preserved', async () => {
  const view = mount([message({text: 'Hello'})])
  try {
    assert.equal(textOf(byClass(view.root, 'msg-role')), '用户')
    view.state.messages[0].role = 'assistant'
    await vue.nextTick()
    assert.equal(textOf(byClass(view.root, 'msg-role')), 'Bot')
    assert.equal(textOf(byClass(view.root, 'msg-bot-label')), '机器人')
    view.state.botName = 'New Bot'
    await vue.nextTick()
    assert.equal(textOf(byClass(view.root, 'msg-role')), 'New Bot')
    assert.equal(view.counts.parse, 1)
    find(view.root, node => node.props.title === '编辑').props.onClick()
    find(view.root, node => node.props.title === '删除').props.onClick()
    assert.deepEqual(view.events, [['edit', 1], ['delete', 1]])
    delete view.state.messages[0].id
    await vue.nextTick()
    assert.equal(byClass(view.root, 'msg-actions'), undefined)
  } finally { view.unmount() }
})
