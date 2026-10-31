import assert from 'node:assert/strict'
import {readFileSync} from 'node:fs'
import {test} from 'node:test'

const css = readFileSync(new URL('../src/style.css', import.meta.url), 'utf8')
const declarations = block => Object.fromEntries([...block.matchAll(/(--[\w-]+):\s*([^;]+);/g)].map(([, key, value]) => [key, value.trim()]))
const light = declarations(css.match(/:root\s*\{([^}]+)\}/)[1])
const dark = {...light, ...declarations(css.match(/:root\[data-theme='dark'\]\s*\{([^}]+)\}/)[1])}
function color(theme, token) {
  let value = theme[token] ?? token
  while (value.startsWith('var(')) value = theme[value.slice(4, -1)]
  if (value.startsWith('#')) {
    let hex = value.slice(1)
    if (hex.length === 3) hex = [...hex].map(c => c + c).join('')
    return [0, 2, 4].map(i => parseInt(hex.slice(i, i + 2), 16)).concat(1)
  }
  const parts = value.match(/[\d.]+/g).map(Number)
  return parts.length === 3 ? [...parts, 1] : parts
}
function over(foreground, background) {
  const alpha = foreground[3]
  return foreground.slice(0, 3).map((channel, i) => channel * alpha + background[i] * (1 - alpha)).concat(1)
}
function luminance(value) {
  return value.slice(0, 3).map(channel => {
    const v = channel / 255
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4
  }).reduce((sum, channel, i) => sum + channel * [0.2126, 0.7152, 0.0722][i], 0)
}
function contrast(foreground, background) {
  const values = [luminance(over(foreground, background)), luminance(background)].sort((a, b) => a - b)
  return (values[1] + 0.05) / (values[0] + 0.05)
}
function meets(foreground, background, minimum, description) {
  const ratio = contrast(foreground, background)
  assert.ok(ratio >= minimum, `${description}: ${ratio.toFixed(2)}:1 < ${minimum}:1`)
}

for (const [name, theme] of [['light', light], ['dark', dark]]) {
  const surfaces = ['--bg-main', '--bg-secondary', '--bg-sidebar', '--card-bg', '--input-bg', '--code-bg']
  test(`${name}: body, secondary and hint text remain readable on ordinary surfaces and row highlights`, () => {
    for (const surface of surfaces) {
      const backgrounds = [color(theme, surface), over(color(theme, '--row-hover'), color(theme, surface))]
      for (const background of backgrounds) {
        for (const token of ['--text-primary', '--text-secondary', '--text-light']) meets(color(theme, token), background, 4.5, `${token} on ${surface}`)
      }
    }
  })
  test(`${name}: semantic text meets contrast on its matching tinted background`, () => {
    for (const surface of surfaces) {
      for (const token of ['primary', 'success', 'warning', 'danger']) {
        const background = over(color(theme, `--${token}-soft`), color(theme, surface))
        meets(color(theme, `--${token}`), background, 4.5, `${token} on ${token}-soft / ${surface}`)
      }
    }
  })
  test(`${name}: enabled button labels meet contrast in default and hover states`, () => {
    for (const token of ['--primary-strong', '--primary-hover', '--success-btn', '--danger-btn', '--warning-btn']) meets(color(theme, '--on-primary'), color(theme, token), 4.5, `white label on ${token}`)
    for (const token of ['--success-btn', '--warning-btn']) {
      const background = color(theme, token).map((channel, i) => i < 3 ? channel * 0.94 : channel)
      meets(color(theme, '--on-primary'), background, 4.5, `hover label on ${token}`)
    }
  })
  test(`${name}: input boundaries and focus indicators remain visible`, () => {
    for (const surface of surfaces) meets(color(theme, '--focus-ring'), color(theme, surface), 3, `focus on ${surface}`)
    for (const surface of ['--card-bg', '--input-bg']) meets(color(theme, '--control-border'), color(theme, surface), 3, `control border on ${surface}`)
  })
  test(`${name}: selections and notifications retain readable text`, () => {
    meets(color(theme, '--selection-text'), color(theme, '--selection-bg'), 4.5, 'selected text')
    meets(color(theme, '--toast-text'), color(theme, '--toast-bg'), 4.5, 'notification text')
  })
}

for (const [name, theme] of [['light', light], ['dark', dark]]) {
  test(`${name}: actual login button and request-tag CSS use readable foreground/background pairs`, () => {
    const app = readFileSync(new URL('../src/App.vue', import.meta.url), 'utf8')
    const button = app.match(/\.login-button\s*\{([^}]+)\}/)[1]
    const foreground = button.match(/color:\s*([^;]+);/)[1]
    const background = button.match(/background:\s*([^;]+);/)[1]
    meets(color(theme, foreground), color(theme, background), 4.5, 'login button')
    const debug = readFileSync(new URL('../src/features/diagnostics/RequestDebug.vue', import.meta.url), 'utf8')
    const tag = debug.match(/\.tag\s*\{([^}]+)\}/)[1]
    const tagText = color(theme, tag.match(/color:\s*([^;]+);/)[1])
    const tagFill = color(theme, tag.match(/background:\s*([^;]+);/)[1])
    meets(tagText, over(tagFill, color(theme, '--card-bg')), 4.5, 'request tag')
  })
  test(`${name}: active and inactive switch tracks remain distinct from the white thumb`, () => {
    for (const token of ['--primary-strong', '--border-strong']) {
      meets(color(theme, '--on-primary'), color(theme, token), 3, `switch thumb on ${token}`)
      meets(color(theme, token), color(theme, '--card-bg'), 3, `switch track on card`)
    }
  })
}
