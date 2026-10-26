import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {test} from 'node:test'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

const require = createRequire(import.meta.url)
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')

// Run actual setup computations without mounted network requests or browser UI.
function setup(page, fetch = () => { throw new Error('Unexpected request') }) {
  const executions = new WeakMap()
  const modules = new Map()
  let conversions = 0
  const api = {
    ...vue, onMounted() {}, onUnmounted() {},
    inject: (key, fallback) => key === 'showToast' ? () => {} : fallback,
    computed(getter) {
      const result = vue.computed(() => {
        executions.set(result, (executions.get(result) || 0) + 1)
        return getter()
      })
      return result
    }
  }
  function load(path) {
    if (modules.has(path)) return modules.get(path).exports
    const module = {exports: {}}
    modules.set(path, module)
    const source = readFileSync(path, 'utf8')
    const script = path.endsWith('.vue') ? compileScript(parse(source).descriptor, {id: path}).content : source
    const {outputText} = ts.transpileModule(script, {
      compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
    })
    new Function('require', 'module', 'exports', 'BigInt', 'fetch', 'document', outputText)(specifier => {
      if (specifier === 'vue') return api
      if (!specifier.startsWith('.')) return require(specifier)
      if (specifier.endsWith('.vue')) return {default: {}}
      return load(resolve(dirname(path), `${specifier}.ts`))
    }, module, module.exports, value => { conversions++; return BigInt(value) }, fetch, {querySelectorAll: () => []})
    return module.exports
  }
  const scope = vue.effectScope()
  const raw = scope.run(() => load(resolve(root, `src/features/${page}.vue`)).default.setup({}, {expose() {}}))
  return {
    state: vue.proxyRefs(raw), runs: name => executions.get(raw[name]) || 0,
    conversions: () => conversions, dispose: () => scope.stop()
  }
}

test('700+ people retain search and numeric ordering while searches reuse cached sorting', () => {
  const view = setup('conversation/PeopleManager')
  try {
    const people = Array.from({length: 700}, (_, i) => ({
      qq: String(9007199254740000n + BigInt(699 - i) * 2n), name: `Person ${i}`,
      affinity: i % 11 - 5, name_pending: false, pending_count: 0, updated_at: ''
    }))
    people.push(...['10', '2'].map(qq => ({...people[0], qq})))
    const originalOrder = people.map(person => person.qq)
    view.state.people = people
    const expected = () => people.filter(person =>
      `${person.qq} ${person.name}`.toLowerCase().includes(view.state.search.trim().toLowerCase()))
      .sort((a, b) => (view.state.ascending ? a.affinity - b.affinity : b.affinity - a.affinity) ||
        (BigInt(a.qq) < BigInt(b.qq) ? -1 : BigInt(a.qq) > BigInt(b.qq) ? 1 : 0))
      .map(person => person.qq)
    for (const ascending of [false, true]) {
      view.state.ascending = ascending
      for (const search of ['', 'person 2', '  PERSON 12  ', '900719925474', 'none']) {
        view.state.search = search
        assert.deepEqual(view.state.filtered.map(person => person.qq), expected())
      }
    }
    assert.equal(view.runs('sortedPeople'), 2)
    assert.equal(view.runs('indexedPeople'), 1)
    assert.equal(view.conversions(), people.length)
    assert.deepEqual(people.map(person => person.qq), originalOrder)

    const person = view.state.people[0]
    person.name = 'Renamed'
    view.state.search = 'renamed'
    assert.equal(view.state.filtered[0], person)
    assert.equal(view.runs('sortedPeople'), 2)
    assert.equal(view.conversions(), people.length)
    person.affinity = 100
    view.state.search = ''
    assert.equal(view.state.filtered.at(-1), person)
    assert.equal(view.runs('sortedPeople'), 3)
    assert.equal(view.conversions(), people.length)

    person.qq = '9007199254740993'
    assert.deepEqual(view.state.filtered.map(person => person.qq), expected())
    assert.equal(view.runs('indexedPeople'), 2)
    assert.equal(view.conversions(), people.length * 2)
    view.state.people = []
    assert.deepEqual(view.state.filtered, [])
    view.state.people = [{...people[0], qq: '7'}]
    assert.equal(view.state.filtered[0].qq, '7')
  } finally { view.dispose() }
})

test('memory settings save the affinity switch as a boolean with the other parameters', async () => {
  let saved
  const view = setup('conversation/MemoryConfig', async (url, options) => {
    assert.equal(url, '/admin/api/memory-config')
    saved = JSON.parse(options.body)
    return {json: async () => ({success: true})}
  })
  try {
    assert.equal(view.state.memoryConfig.affinityEnabled, true)
    view.state.memoryConfig.affinityEnabled = false
    await view.state.saveMemoryConfig()
    assert.equal(saved.affinityEnabled, false)
    assert.equal(saved.memorySummaryBatchSize, 50)
    assert.equal(view.state.saving, false)
  } finally { view.dispose() }
})

test('tool grouping preserves category and tool order, global counts, and existing search rules', () => {
  const view = setup('tools/CustomTools')
  try {
    const builtin = (name, category, enabled = true) => ({
      name, category, enabled, description: 'Original description', defaultDescription: '', customDescription: false
    })
    view.state.builtinTools = [builtin('replyB', '回复'), builtin('action', '动作', false),
      builtin('replyA', '回复'), builtin('unknown', '其他')]
    view.state.tools = [{id: 1, name: 'LuaOne', description: ' Useful tool ', enabled: true},
      {id: 2, name: 'LuaTwo', description: 'Other', enabled: false}]
    assert.deepEqual(view.state.builtinGroups.map(group => [group.category, group.tools.map(tool => tool.name)]),
      [['回复', ['replyB', 'replyA']], ['信息', []], ['动作', ['action']]])
    assert.equal(view.state.builtinGroups[0].tools[0], view.state.builtinTools[0])
    assert.equal(view.state.enabledBuiltinCount, 3)
    assert.equal(view.state.enabledCustomCount, 1)
    const groups = view.state.builtinGroups
    view.state.editingBuiltin = {...view.state.builtinTools[0]}
    view.state.pendingTools.add('builtin:replyB')
    view.state.builtinTools[0].enabled = false
    assert.equal(view.state.enabledBuiltinCount, 2)
    assert.equal(view.state.builtinGroups, groups)
    assert.equal(view.runs('builtinGroups'), 1)

    view.state.builtinFilter = 'REPLY'
    assert.equal(view.state.builtinGroups[0].tools.length, 2)
    assert.equal(view.state.enabledBuiltinCount, 2)
    assert.equal(view.runs('enabledBuiltinCount'), 2)
    view.state.builtinFilter = ' reply '
    assert.equal(view.state.visibleBuiltins.length, 0)
    view.state.builtinFilter = 'new description'
    view.state.builtinTools[1].description = 'New description'
    assert.deepEqual(view.state.builtinGroups[2].tools.map(tool => tool.name), ['action'])
    view.state.builtinTools[1].category = '信息'
    assert.deepEqual(view.state.builtinGroups[1].tools.map(tool => tool.name), ['action'])

    view.state.customFilter = 'LUAONE'
    assert.equal(view.state.visibleCustomTools[0], view.state.tools[0])
    view.state.customFilter = ' useful '
    assert.equal(view.state.visibleCustomTools.length, 1)
    view.state.tools[0].description = 'Changed'
    assert.equal(view.state.visibleCustomTools.length, 0)
    view.state.tools[1].enabled = true
    assert.equal(view.state.enabledCustomCount, 2)
    view.state.builtinTools = []
    view.state.tools = []
    assert.equal(view.state.builtinGroups.length, 3)
    assert.ok(view.state.builtinGroups.every(group => group.tools.length === 0))
    assert.equal(view.state.enabledBuiltinCount, 0)
    assert.equal(view.state.enabledCustomCount, 0)
  } finally { view.dispose() }
})

test('usage totals, cache rates and merged role data are unchanged after removing unused state', async () => {
  const role = (role, calls, prompt, cached, total) => ({role, model: role, calls, prompt, cached, total, completion: total - prompt})
  const data = {
    total_calls: 12, total_tokens: 5000, total_prompt: 3700, total_completion: 1300, total_cached: 1800,
    today: {calls: 8, prompt: 1399, completion: 351, total: 1750, cached: 1189},
    today_by_role: [role('router', 2, 100, 40, 150), role('executor', 4, 300, 150, 400), role('memory', 2, 999, 999, 1200)],
    by_role: [role('router', 4, 1000, 600, 1400), role('executor', 6, 2000, 500, 2600), role('memory', 2, 700, 700, 1000)],
    by_day: [{day: '2026-10-20', calls: 6, total: 2000}, {day: '2026-10-21', calls: 6, total: 3000}],
    recent: [role('router', 1, 100, 40, 150)]
  }
  const view = setup('llm/UsageStats', async () => ({ok: true, json: async () => data}))
  try {
    await view.state.loadUsage()
    assert.equal(view.state.totalCalls, 12)
    assert.equal(view.state.totalTokens, 5000)
    assert.equal(view.state.todayTotalCalls, 8)
    assert.equal(view.state.todayTotalTokens, 1750)
    assert.equal(view.state.todayPromptTotal, 1399)
    assert.equal(view.state.todayCompletionTotal, 351)
    assert.equal(view.state.todayHitRate, 47.5)
    assert.equal(view.state.monthHitRate, 1100 / 3000 * 100)
    assert.equal(view.state.dailyAvgTokens, 2500)
    assert.equal(view.state.maxDayTotal, 3000)
    assert.deepEqual(view.state.recent, data.recent)
    assert.deepEqual(view.state.mergedRoles.map(item => [item.role, item.todayCalls, item.todayTokens,
      item.todayCacheRate, item.periodCalls, item.periodTokens, item.share, item.avgPerCall, item.cacheRate]), [
      ['router', 2, 150, '40.0%', 4, 1400, 1400 / 5000 * 100, 350, '60.0%'],
      ['executor', 4, 400, '50.0%', 6, 2600, 52, 433, '25.0%'],
      ['memory', 2, 1200, '—', 2, 1000, 20, 500, '—']
    ])
    for (const key of ['totalPrompt', 'totalCompletion', 'totalCached', 'dailyAvgCalls'])
      assert.equal(Object.hasOwn(view.state, key), false)
  } finally { view.dispose() }
})

test('session helpers preserve large IDs and each page keeps its own fallback labels', () => {
  const groupView = setup('conversation/GroupManager')
  const logView = setup('diagnostics/LogViewer')
  const requestView = setup('diagnostics/RequestDebug')
  const peopleView = setup('conversation/PeopleManager')
  try {
    const group = {groupId: 123, groupName: 'Named group'}
    const privateSession = {groupId: 0, groupIdStr: '9223372036854775931',
      groupName: 'Alice', sessionType: 'private', userId: 123}
    assert.equal(groupView.state.sessionKey(privateSession), privateSession.groupIdStr)
    assert.equal(groupView.state.sessionKey(group), '123')
    assert.equal(groupView.state.isPrivateSession(privateSession), true)
    assert.equal(groupView.state.isPrivateSession(group), false)
    for (const view of [groupView, logView]) {
      assert.equal(view.state.sessionLabel(group), 'Named group')
      assert.equal(view.state.sessionLabel({...group, groupName: ''}), '群 123')
      assert.equal(view.state.sessionLabel(privateSession), 'Alice (123)')
      assert.equal(view.state.sessionLabel({...privateSession, groupName: ''}), '私聊 123')
      assert.equal(view.state.sessionLabel({groupId: 0, sessionType: 'private'}), '私聊 ')
    }
    logView.state.groups = [privateSession]
    assert.equal(logView.state.groupTag(privateSession.groupIdStr), 'Alice (123)')
    for (const [id, user] of [['9223372036854775808', '0'], ['9223372036854775931', '123'],
      ['18446744073709551615', '9223372036854775807']]) {
      logView.state.groups = []
      assert.equal(logView.state.groupTag(id), `私聊 ${user}`)
      assert.equal(requestView.state.sessionLabel(id), `私聊 ${user}`)
      assert.equal(peopleView.state.sourceLabel(id), `私聊 ${user}`)
    }
    for (const id of ['0', '123', '9007199254740993']) {
      assert.equal(logView.state.groupTag(id), `群 ${id}`)
      assert.equal(requestView.state.sessionLabel(id), `群 ${id}`)
      assert.equal(peopleView.state.sourceLabel(id), id === '0' ? '历史记忆迁移' : `群聊 ${id}`)
    }
    for (const id of [null, undefined, '']) assert.equal(requestView.state.sessionLabel(id), '系统请求')
    assert.equal(requestView.state.sessionLabel('invalid'), 'invalid')
    assert.equal(logView.state.groupTag('invalid'), '群 invalid')
    assert.throws(() => peopleView.state.sourceLabel('invalid'), SyntaxError)
  } finally {
    for (const view of [groupView, logView, requestView, peopleView]) view.dispose()
  }
})

test('opening a new custom tool restores all defaults without replacing the reactive draft', async () => {
  const view = setup('tools/CustomTools')
  try {
    const original = view.state.editTool
    const defaults = {...original}
    const first = view.state.createDefaultTool()
    const second = view.state.createDefaultTool()
    assert.notEqual(first, second)
    assert.deepEqual(first, defaults)
    for (let i = 0; i < 2; i++) {
      view.state.openEditDialog({id: 99, name: 'Old', description: 'Changed', parameters: '[]',
        executorType: 'lua', scriptContent: 'old', readme: 'Old readme', enabled: false})
      view.state.testArgs = '{"old":true}'
      view.state.testResult = 'old result'
      view.state.openAddDialog()
      await vue.nextTick()
      assert.equal(view.state.editTool, original)
      assert.deepEqual(view.state.editTool, defaults)
      assert.equal(view.state.isEdit, false)
      assert.equal(view.state.showDialog, true)
      assert.equal(view.state.testArgs, '{}')
      assert.equal(view.state.testResult, '')
    }
  } finally { view.dispose() }
})
