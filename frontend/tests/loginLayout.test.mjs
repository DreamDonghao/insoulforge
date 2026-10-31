import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileTemplate, parse} from 'vue/compiler-sfc'
import {renderToString} from 'vue/server-renderer'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/App.vue', import.meta.url), 'utf8')
const {descriptor} = parse(source)
const compiled = compileTemplate({source: descriptor.template.content, filename: 'App.vue',
  id: 'login-test', compilerOptions: {mode: 'function'}})
assert.deepEqual(compiled.errors, [])
const render = new Function('Vue', compiled.code)(vue)
const components = Object.fromEntries([...descriptor.template.content.matchAll(/<([A-Z]\w*)/g)]
  .map(([, name]) => [name, {render: () => null}]))

function page() {
  let tree, submits = 0
  const state = vue.reactive({
    authenticationChecked: false, authenticated: false, theme: 'light',
    get themeToggleLabel() { return this.theme === 'dark' ? '切换为浅色模式' : '切换为深色模式' },
    accessToken: '', loginError: '', loginSubmitting: false, showAccessToken: false,
    notifications: [], toastTarget: null, dismissToast() {}, currentView: 'dashboard', dailyNavItems: [], systemNavItems: [],
    sidebarNav: null, navIndicator: {height: 0, top: 0}, avatarUrl: '', avatarFailed: false,
    qqConfig: {botName: 'Bot'}, resetPageScroll() {},
    changeView: view => { state.currentView = view },
    login: () => { submits++ }, logout: () => { state.authenticated = false },
    toggleTheme: () => { state.theme = state.theme === 'dark' ? 'light' : 'dark' }
  })
  return {
    state, submits: () => submits,
    async html() {
      return renderToString(vue.createSSRApp({
        components, setup: () => state,
        render(...args) { tree = render.apply(this, args); return tree }
      }))
    },
    find(predicate) {
      function visit(node) {
        if (!node || typeof node !== 'object') return
        if (predicate(node)) return node
        if (Array.isArray(node.children)) return node.children.map(visit).find(Boolean)
      }
      return visit(tree)
    }
  }
}

test('pending authentication and login form share exactly one identity and theme control', async () => {
  const view = page()
  for (const theme of ['light', 'dark']) {
    view.state.theme = theme
    for (const checking of [true, false]) {
      view.state.authenticationChecked = !checking
      const html = await view.html()
      for (const name of ['login-page', 'login-shell', 'login-identity', 'login-product-name', 'login-theme-toggle'])
        assert.equal(html.split(`class="${name}"`).length - 1, 1, name)
      assert.equal(html.includes('正在验证登录状态'), checking)
      assert.equal(html.includes('<form'), !checking)
      assert.ok(html.includes(theme === 'dark' ? '切换为浅色模式' : '切换为深色模式'))
      assert.ok(html.includes('InSoulForge'))
    }
  }
  view.state.authenticationChecked = false
  view.state.authenticated = true
  const pending = await view.html()
  assert.ok(pending.includes('正在验证登录状态'))
  assert.equal(pending.includes('class="container"'), false)
})

test('login controls preserve theme, token visibility, validation state and submit handling', async () => {
  const view = page()
  view.state.authenticationChecked = true
  await view.html()
  const button = () => view.find(node => node.props?.class === 'login-button')
  const input = () => view.find(node => node.props?.id === 'access-token')
  assert.equal(button().props.disabled, true)
  assert.equal(input().props.type, 'password')
  view.find(node => node.props?.class === 'login-theme-toggle').props.onClick()
  assert.equal(view.state.theme, 'dark')
  view.find(node => node.props?.class === 'input-icon-button').props.onClick()
  view.state.accessToken = 'test-token'
  await view.html()
  assert.equal(input().props.type, 'text')
  assert.equal(button().props.disabled, false)
  let prevented = false
  view.find(node => node.type === 'form').props.onSubmit({preventDefault() { prevented = true }})
  assert.equal(prevented, true)
  assert.equal(view.submits(), 1)
  view.state.loginSubmitting = true
  view.state.loginError = '访问令牌无效'
  const html = await view.html()
  assert.equal(input().props.disabled, true)
  assert.equal(button().props.disabled, true)
  assert.ok(html.includes('验证中...'))
  assert.ok(html.includes('role="alert">访问令牌无效'))
})

test('authenticated content excludes the login shell and logout returns to the same form', async () => {
  const view = page()
  view.state.authenticationChecked = true
  view.state.authenticated = true
  const html = await view.html()
  assert.ok(html.includes('class="container"'))
  assert.equal(html.includes('class="login-page"'), false)
  view.state.logout()
  const login = await view.html()
  assert.ok(login.includes('<form'))
  assert.equal(login.split('class="login-identity"').length - 1, 1)
})

test('compact navigation retains names, button behavior and an accurate item count', async () => {
  const view = page()
  view.state.authenticationChecked = true
  view.state.authenticated = true
  view.state.dailyNavItems = [{key: 'dashboard', label: '首页', icon: 'home'},
    {key: 'people', label: '人物信息', icon: 'admins'}]
  view.state.systemNavItems = [{key: 'llm', label: '模型配置', icon: 'llm'}]
  await view.html()
  const sidebar = () => view.find(node => node.props?.class === 'sidebar')
  assert.equal(sidebar().props.style['--nav-item-count'], 4)
  assert.equal(view.find(node => node.props?.['aria-label'] === '退出登录').props.title, '退出登录')
  for (const [label, key] of [['人物信息', 'people'], ['模型配置', 'llm'], ['关于', 'about']]) {
    const button = view.find(node => node.props?.['aria-label'] === label)
    assert.equal(button.type, 'button')
    assert.equal(button.props.type, 'button')
    assert.equal(button.props.title, label)
    button.props.onClick()
    assert.equal(view.state.currentView, key)
  }
  view.state.dailyNavItems.push({key: 'groups', label: '会话管理', icon: 'groups'})
  await view.html()
  assert.equal(sidebar().props.style['--nav-item-count'], 5)
})
