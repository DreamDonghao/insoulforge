import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileScript, compileTemplate, parse} from 'vue/compiler-sfc'
import {renderToString} from 'vue/server-renderer'
import ts from 'typescript'
import * as vue from 'vue'

const {descriptor} = parse(readFileSync(new URL('../src/features/llm/LLMModelForm.vue', import.meta.url), 'utf8'))
const script = compileScript(descriptor, {id: 'llm-model-test'}).content
const {outputText} = ts.transpileModule(script, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
})
const template = compileTemplate({source: descriptor.template.content, filename: 'LLMModelForm.vue',
  id: 'llm-model-test', compilerOptions: {mode: 'function'}})
assert.deepEqual(template.errors, [])
const render = new Function('Vue', template.code)(vue)

function form(name = 'imageGeneration', overrides = {}) {
  const requests = [], events = []
  const module = {exports: {}}
  new Function('require', 'module', 'exports', 'fetch', outputText)(() => ({...vue, inject: () => () => {}}),
    module, module.exports, async (url, options) => {
      requests.push({url, body: JSON.parse(options.body)})
      return {ok: true, json: async () => ({success: true})}
    })
  const props = vue.reactive({name, initialConfig: {
    apiKey: 'fixture', baseUrl: 'https://proxy.example.com/v1', path: '/images/generations',
    model: 'gpt-image-2.5-sunburst', apiType: 'openai', editPath: '/images/edits', ...overrides
  }})
  const scope = vue.effectScope()
  const state = vue.proxyRefs(scope.run(() => module.exports.default.setup(props, {
    expose() {}, emit: (...args) => events.push(args)
  })))
  return {state, props, requests, events, dispose: () => scope.stop(),
    html: () => renderToString(vue.createSSRApp({setup: () => ({...state, name, title: undefined}), render}))}
}

test('image API selector switches standard paths but preserves custom paths, credentials and edit path', async () => {
  const view = form()
  try {
    let html = await view.html()
    assert.ok(html.includes('OpenAI / CLIProxyAPI') && html.includes('编辑路径（Edit Path）'))
    view.state.llmConfig.apiType = 'openrouter'
    await vue.nextTick()
    assert.equal(view.state.llmConfig.path, '/images')
    html = await view.html()
    assert.equal(html.includes('编辑路径（Edit Path）'), false)
    view.state.llmConfig.apiType = 'openai'
    await vue.nextTick()
    assert.equal(view.state.llmConfig.path, '/images/generations')
    view.state.llmConfig.path = '/custom/generation'
    view.state.llmConfig.apiType = 'openrouter'
    await vue.nextTick()
    assert.equal(view.state.llmConfig.path, '/custom/generation')
    assert.equal(view.state.llmConfig.baseUrl, 'https://proxy.example.com/v1')
    assert.equal(view.state.llmConfig.apiKey, 'fixture')
    assert.equal(view.state.llmConfig.editPath, '/images/edits')
    await view.state.saveLLMConfig()
    assert.equal(view.requests[0].url, '/admin/api/llm-config')
    assert.equal(view.requests[0].body.apiType, 'openrouter')
    assert.equal(view.requests[0].body.editPath, '/images/edits')
    assert.equal(view.events[0][0], 'saved')
  } finally { view.dispose() }
})

test('chat model forms do not expose image API controls or rewrite paths', async () => {
  const view = form('executor', {path: '/chat/completions'})
  try {
    const html = await view.html()
    assert.equal(html.includes('OpenAI / CLIProxyAPI'), false)
    view.state.llmConfig.apiType = 'openrouter'
    await vue.nextTick()
    assert.equal(view.state.llmConfig.path, '/chat/completions')
  } finally { view.dispose() }
})
