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

test('all model forms show and save independent request timeout and retry settings', async () => {
  for (const name of ['router', 'executor', 'executorFallback', 'executorThinking', 'image',
    'memory', 'imageGeneration', 'embedding', 'jev']) {
    const view = form(name, {timeoutSeconds: 180, maxRetries: 0})
    try {
      const html = await view.html()
      assert.ok(html.includes('请求超时（秒）') && html.includes('失败重试次数'))
      assert.ok(html.includes('min="1"') && html.includes('max="3600"') && html.includes('max="10"'))
      assert.equal(html.includes('可能重复生成并计费'), name === 'imageGeneration')
      view.state.llmConfig.timeoutSeconds = 600
      view.state.llmConfig.maxRetries = 2
      await view.state.saveLLMConfig()
      assert.equal(view.requests[0].body.timeoutSeconds, 600)
      assert.equal(view.requests[0].body.maxRetries, 2)
      assert.equal(view.requests[0].body.name, name)
      view.props.initialConfig = {...view.props.initialConfig, timeoutSeconds: 45, maxRetries: 1}
      await vue.nextTick()
      assert.equal(view.state.llmConfig.timeoutSeconds, 45)
      assert.equal(view.state.llmConfig.maxRetries, 1)
    } finally { view.dispose() }
  }
})
