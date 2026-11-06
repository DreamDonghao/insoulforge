import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import {compileTemplate, parse} from 'vue/compiler-sfc'
import {renderToString} from 'vue/server-renderer'
import * as vue from 'vue'

const source = readFileSync(new URL('../src/App.vue', import.meta.url), 'utf8')
const desktopTemplate = parse(readFileSync(new URL('../src/desktop/DesktopApp.vue', import.meta.url), 'utf8')).descriptor.template.content
const loginTemplate = parse(readFileSync(new URL('../src/desktop/DesktopLogin.vue', import.meta.url), 'utf8')).descriptor.template.content
const expandedSource = source.replace(/<DesktopLogin[^>]+\/>/, loginTemplate.replace('<main', '<main v-else-if="!authenticationChecked || !authenticated"'))
const {descriptor} = parse(expandedSource.replace('<DesktopApp :state="desktopState"/>', desktopTemplate))
const compiled = compileTemplate({source: descriptor.template.content, filename: 'App.vue',
  id: 'login-test', compilerOptions: {mode: 'function'}})
assert.deepEqual(compiled.errors, [])
const render = new Function('Vue', compiled.code)(vue)
const components = Object.fromEntries([...descriptor.template.content.matchAll(/<([A-Z]\w*)/g)]
  .map(([, name]) => [name, {render: () => null}]))
components.ModalDialog = {render() { return vue.h('div', {role: 'dialog'}, this.$slots.default?.()) }}

function page() {
  let tree, submits = 0
  const state = vue.reactive({
    authenticationChecked: false, authenticated: false, theme: 'light', accentColor: '#16746f', accentStorageError: '', setAccentColor() {},
    get themeToggleLabel() { return this.theme === 'dark' ? '切换为浅色模式' : '切换为深色模式' },
    accessToken: '', loginError: '', loginSubmitting: false, showAccessToken: false,
    logoutConfirmationVisible: false, logoutSubmitting: false, logoutError: '', logoutHasUnsavedChanges: () => false,
    notifications: [], toastTarget: null, dismissToast() {}, currentView: 'dashboard', dailyNavItems: [], systemNavItems: [],
    sidebarNav: null, navIndicator: {height: 0, top: 0}, avatarUrl: '', avatarFailed: false,
    qqConfig: {botName: 'Bot'}, resetPageScroll() {}, isMobile: false, mobileNavigationOpen: false, ModalDialog: components.ModalDialog,
    changeView: view => { state.currentView = view },
    login: () => { submits++ }, logout: () => { state.logoutConfirmationVisible = true },
    cancelLogout: () => { state.logoutConfirmationVisible = false },
    confirmLogout: () => { state.logoutConfirmationVisible = false; state.authenticated = false },
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
        if (Array.isArray(node)) return node.map(visit).find(Boolean)
        if (predicate(node)) return node
        if (Array.isArray(node.children)) return node.children.map(visit).find(Boolean)
        if (typeof node.children?.default === 'function') return visit(node.children.default())
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

test('logout presents confirmation and cancellation preserves the page before explicit confirmation', async () => {
  const view = page()
  view.state.authenticationChecked = true
  view.state.authenticated = true
  const html = await view.html()
  assert.ok(html.includes('class="container"'))
  assert.equal(html.includes('class="login-page"'), false)
  view.state.logout()
  view.state.logoutHasUnsavedChanges = () => true
  const confirmation = await view.html()
  assert.ok(confirmation.includes('有未保存的修改'))
  const cancel = view.find(node => node.props?.['data-modal-initial-focus'] !== undefined)
  assert.equal(cancel.props.type, 'button')
  cancel.props.onClick()
  assert.equal(view.state.authenticated, true)
  assert.equal(view.state.logoutConfirmationVisible, false)
  view.state.logout()
  view.state.confirmLogout()
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

test('theme settings use normal page navigation and show an active sidebar entry', async () => {
  const view = page()
  view.state.authenticationChecked = true
  view.state.authenticated = true
  view.state.currentView = 'llm'
  await view.html()
  const button = view.find(node => node.props?.['aria-label'] === '主题配色')
  assert.equal(button.props.type, 'button')
  button.props.onClick()
  assert.equal(view.state.currentView, 'themeSettings')
  await view.html()
  const selected = view.find(node => node.props?.['aria-label'] === '主题配色')
  assert.equal(selected.props['aria-current'], 'page')
  assert.match(selected.props.class, /active/)
})
