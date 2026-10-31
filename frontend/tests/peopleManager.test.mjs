import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileScript, compileTemplate, parse} from 'vue/compiler-sfc'
import {renderToString} from 'vue/server-renderer'
import ts from 'typescript'
import * as vue from 'vue'

const {descriptor} = parse(readFileSync(new URL('../src/features/conversation/PeopleManager.vue', import.meta.url), 'utf8'))
const script = compileScript(descriptor, {id: 'people-test'}).content
const {outputText} = ts.transpileModule(script, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
})
const template = compileTemplate({source: descriptor.template.content, filename: 'PeopleManager.vue',
  id: 'people-test', compilerOptions: {mode: 'function'}})
assert.deepEqual(template.errors, [])
const render = new Function('Vue', template.code)(vue)
const person = () => ({qq: '42', name: '测试人物', affinity: 7, profile: ['喜欢飞钓'],
  relationship: ['经常闲聊'], recent_interactions: [], pending_entries: [], name_pending: false})

function page(post, settings = {affinityEnabled: true}) {
  const requests = [], module = {exports: {}}
  const require = name => name === 'vue' ? {...vue, onMounted() {}, onUnmounted() {}} :
    name.includes('useUnsavedChanges') ? {useUnsavedChanges() {}, confirmDiscard: () => true} :
    name.includes('session') ? {privateSessionUserId: () => null} :
      Object.fromEntries(['ArrowDownWideNarrow', 'ArrowUpNarrowWide', 'Pencil', 'Trash2'].map(key => [key, () => null]))
  new Function('require', 'module', 'exports', 'fetch', outputText)(require, module, module.exports,
    async (url, options = {}) => {
      requests.push({url, ...options})
      const current = person()
      if (!settings.affinityEnabled) delete current.affinity
      const result = options.method === 'POST' ? await post() :
        {status: 200, body: url === '/admin/api/memory-config' ? settings :
          url === '/admin/api/people' ? [{...current, pending_count: 0}] : current}
      return {ok: result.status === 200, status: result.status,
        headers: new Headers({'content-type': 'application/json'}),
        json: async () => result.body, text: async () => JSON.stringify(result.body)}
    })
  const scope = vue.effectScope()
  const state = vue.proxyRefs(scope.run(() => module.exports.default.setup({}, {expose() {}})))
  state.people = [{...person(), pending_count: 0}]
  state.detail = person()
  state.selected = '42'
  state.affinityEnabled = settings.affinityEnabled
  if (!settings.affinityEnabled) delete state.detail.affinity
  return {state, requests, settings, dispose: () => scope.stop(),
    html: () => renderToString(vue.createSSRApp({setup: () => ({...state}), render,
      components: require('@lucide/vue')}))}
}

test('manual consolidation prevents duplicate submissions and edits, then refreshes the selected profile', async () => {
  let finish
  const view = page(() => new Promise(resolve => { finish = resolve }))
  try {
    const task = view.state.consolidatePerson()
    assert.equal(view.state.consolidating, true)
    await view.state.consolidatePerson()
    await view.state.selectPerson('99')
    view.state.beginEdit()
    assert.equal(view.state.selected, '42')
    assert.equal(view.state.editing, false)
    assert.equal(view.requests.length, 1)
    const busyHtml = await view.html()
    assert.ok(busyHtml.includes('整理中...') && busyHtml.includes('人物档案') && busyHtml.includes('与机器人的关系'))
    finish({status: 200, body: {success: true, message: '人物记忆已整理'}})
    await task
    assert.equal(view.state.consolidating, false)
    assert.equal(view.state.consolidationMessage, '人物记忆已整理')
    assert.deepEqual(view.requests.map(({url}) => url), [
      '/admin/api/people/42/consolidate', '/admin/api/people', '/admin/api/memory-config', '/admin/api/people/42'])
    view.state.beginEdit()
    const draft = JSON.parse(view.state.draftText)
    assert.deepEqual(draft.relationship, ['经常闲聊'])
    assert.equal('stable_memory' in draft, false)
    await view.state.consolidatePerson()
    assert.equal(view.requests.length, 4)
  } finally { view.dispose() }
})

test('manual consolidation errors preserve displayed memory and show the backend reason', async () => {
  const view = page(async () => ({status: 409, body: {error: '人物整理正在进行，请稍后重试'}}))
  try {
    await view.state.consolidatePerson()
    assert.equal(view.state.editError, '人物整理正在进行，请稍后重试')
    assert.equal(view.state.consolidating, false)
    assert.deepEqual(view.state.detail.profile, ['喜欢飞钓'])
    assert.equal(view.state.sourceLabel('0'), '历史记忆迁移')
    assert.equal(view.requests.length, 1)
  } finally { view.dispose() }
})

test('disabled affinity hides scores and sorting, edits only memory and orders numerically by QQ', async () => {
  const view = page(async () => ({status: 200, body: {success: true}}), {affinityEnabled: false})
  try {
    view.state.people = [{...person(), qq: '42', affinity: 100}, {...person(), qq: '2', affinity: -100}]
    assert.deepEqual(view.state.sortedPeople.map(person => person.qq), ['2', '42'])
    const html = await view.html()
    assert.equal(html.includes('全局好感度'), false)
    assert.equal(html.includes('好感度：'), false)
    view.state.beginEdit()
    assert.equal('affinity' in JSON.parse(view.state.draftText), false)
    await view.state.savePerson()
    const saved = JSON.parse(view.requests.find(request => request.method === 'PUT').body)
    assert.equal('affinity' in saved.replacement, false)
    assert.deepEqual(saved.replacement.profile, ['喜欢飞钓'])
  } finally { view.dispose() }
})

test('a configuration change during editing discards the old editor without submitting it', async () => {
  const view = page(async () => ({status: 200, body: {success: true}}))
  try {
    view.state.beginEdit()
    view.settings.affinityEnabled = false
    await view.state.savePerson()
    assert.equal(view.requests.some(request => request.method === 'PUT'), false)
    assert.equal(view.state.editing, false)
    assert.equal(view.state.affinityEnabled, false)
    assert.equal(view.state.draftText, '')
    assert.ok(view.state.editError.includes('好感度设置已变化'))
    assert.equal('affinity' in view.state.detail, false)
    view.settings.affinityEnabled = true
    await view.state.loadPeople()
    assert.equal(view.state.affinityEnabled, true)
    assert.equal(view.state.detail.affinity, 7)
  } finally { view.dispose() }
})

test('invalid JSON identifies and focuses its editor, retains the draft and clears validation after correction', async () => {
  const view = page(async () => ({status: 200, body: {success: true}}))
  let focused = 0
  view.state.jsonEditor = {focus() { focused++ }}
  try {
    view.state.beginEdit()
    await vue.nextTick()
    assert.equal(focused, 1)
    const validDraft = view.state.draftText
    view.state.draftText = '{invalid'
    await view.state.savePerson()
    await vue.nextTick()
    assert.equal(focused, 2)
    assert.equal(view.requests.length, 0)
    assert.equal(view.state.draftText, '{invalid')
    assert.equal(view.state.editing, true)
    assert.equal(view.state.jsonInvalid, true)
    const html = await view.html()
    assert.match(html, /id="person-json-error"[^>]*role="alert"/)
    assert.match(html, /<textarea[^>]*id="person-json"[^>]*aria-invalid="true"[^>]*aria-describedby="person-json-error"/)
    view.state.draftText = validDraft
    await view.state.savePerson()
    assert.equal(view.state.jsonInvalid, false)
    assert.equal(view.state.editing, false)
  } finally { view.dispose() }
})
