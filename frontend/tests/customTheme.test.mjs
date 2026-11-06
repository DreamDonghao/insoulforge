import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'
import ts from 'typescript'

const module = {exports: {}}
const source = readFileSync(new URL('../src/utils/theme.ts', import.meta.url), 'utf8')
const code = ts.transpileModule(source, {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}}).outputText
new Function('module', 'exports', code)(module, module.exports)
const {ACCENT_PRESETS, ACCENT_STORAGE_KEY, ACCENT_TOKENS, DEFAULT_ACCENT, normalizeAccent,
  readAccentColor, saveAccentColor, deriveAccentPalette, applyAccentColor} = module.exports
const css = readFileSync(new URL('../src/style.css', import.meta.url), 'utf8')
const declarations = block => Object.fromEntries([...block.matchAll(/(--[\w-]+):\s*([^;]+);/g)].map(([, key, value]) => [key, value.trim()]))
const light = declarations(css.match(/:root\s*\{([^}]+)\}/)[1])
const dark = {...light, ...declarations(css.match(/:root\[data-theme='dark'\]\s*\{([^}]+)\}/)[1])}
const surfaces = ['--bg-main', '--bg-secondary', '--bg-sidebar', '--card-bg', '--input-bg', '--code-bg']
const texts = ['--text-primary', '--text-secondary', '--text-light']
function color(value) {
  if (value.startsWith('#')) return [1, 3, 5].map(index => parseInt(value.slice(index, index + 2), 16)).concat(1)
  return value.match(/[\d.]+/g).map(Number)
}
function over(fg, bg) { return fg.slice(0, 3).map((channel, index) => channel * fg[3] + bg[index] * (1 - fg[3])).concat(1) }
function luminance(rgb) {
  return rgb.slice(0, 3).map(channel => {
    const v = channel / 255
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4
  }).reduce((sum, channel, index) => sum + channel * [0.2126, 0.7152, 0.0722][index], 0)
}
function meets(fg, bg, minimum, message) {
  const [low, high] = [luminance(fg), luminance(bg)].sort((a, b) => a - b)
  const ratio = (high + 0.05) / (low + 0.05)
  assert.ok(ratio >= minimum, `${message}: ${ratio.toFixed(3)} < ${minimum}`)
}

test('custom colors normalize safely; corrupt preferences and unavailable storage preserve the default', () => {
  assert.equal(normalizeAccent(' #AbC '), '#aabbcc')
  assert.equal(normalizeAccent('#3B6FD8'), '#3b6fd8')
  for (const value of ['', 'red', '#gg0000', '#12345678', '#fff; color:red']) assert.equal(normalizeAccent(value), null)
  assert.equal(readAccentColor({getItem: () => '#3B6FD8'}), '#3b6fd8')
  assert.equal(readAccentColor({getItem: () => '{broken'}), DEFAULT_ACCENT)
  assert.equal(readAccentColor({getItem() { throw Error('blocked') }}), DEFAULT_ACCENT)
  const items = new Map()
  const storage = {getItem: key => items.get(key), setItem: (key, value) => items.set(key, value), removeItem: key => items.delete(key)}
  assert.equal(saveAccentColor(storage, '#3b6fd8'), true)
  assert.equal(readAccentColor(storage), '#3b6fd8')
  assert.equal(saveAccentColor(storage, DEFAULT_ACCENT), true)
  assert.equal(items.has(ACCENT_STORAGE_KEY), false)
  assert.equal(saveAccentColor({setItem() { throw Error('quota') }}, '#3b6fd8'), false)
})

for (const [mode, theme] of [['light', light], ['dark', dark]]) {
  test(`${mode}: presets and custom colors retain readable text, controls and selections including black, white and bright colors`, () => {
    const colors = new Set(ACCENT_PRESETS.map(preset => preset.color))
    for (const r of [0, 64, 128, 192, 255]) for (const g of [0, 64, 128, 192, 255]) for (const b of [0, 64, 128, 192, 255]) {
      colors.add('#' + [r, g, b].map(channel => channel.toString(16).padStart(2, '0')).join(''))
    }
    for (const seed of colors) {
      const palette = deriveAccentPalette(seed, mode, surfaces.map(key => theme[key]), theme['--card-bg'], texts.map(key => theme[key]))
      for (const surface of surfaces) {
        const bg = color(theme[surface])
        meets(color(palette['--primary']), bg, 4.5, `${seed} primary/${surface}`)
        meets(color(palette['--focus-ring']), bg, 3, `${seed} focus/${surface}`)
        for (const token of ['--primary-soft', '--primary-softer']) {
          const tinted = over(color(palette[token]), bg)
          meets(color(palette['--primary']), tinted, 4.5, `${seed} primary/${token}/${surface}`)
          for (const text of texts) meets(color(theme[text]), tinted, 4.5, `${seed} ${text}/${token}/${surface}`)
        }
      }
      for (const token of ['--primary-strong', '--primary-hover']) meets(color('#ffffff'), color(palette[token]), 4.5, `${seed} button/${token}`)
      if (mode === 'dark') meets(color(palette['--primary-strong']), color(theme['--card-bg']), 3, `${seed} switch/card`)
      meets(color(palette['--selection-text']), color(palette['--selection-bg']), 4.5, `${seed} text selection`)
    }
  })
}

test('mode changes replace all accent tokens and reset removes only owned overrides', () => {
  const original = globalThis.getComputedStyle
  let theme = light
  globalThis.getComputedStyle = () => ({getPropertyValue: token => theme[token]})
  const values = new Map([['--unrelated', 'keep']])
  const root = {style: {setProperty: (key, value) => values.set(key, value), removeProperty: key => values.delete(key)}}
  try {
    applyAccentColor(root, '#3b6fd8', 'light')
    const lightColor = values.get('--primary')
    assert.ok(ACCENT_TOKENS.every(token => values.has(token)))
    assert.equal(values.has('--danger'), false)
    theme = dark
    applyAccentColor(root, '#3b6fd8', 'dark')
    assert.notEqual(values.get('--primary'), lightColor)
    applyAccentColor(root, DEFAULT_ACCENT, 'dark')
    assert.ok(ACCENT_TOKENS.every(token => !values.has(token)))
    assert.equal(values.get('--unrelated'), 'keep')
  } finally { globalThis.getComputedStyle = original }
})
