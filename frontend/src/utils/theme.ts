export type ThemeMode = 'light' | 'dark'
type RGB = readonly [number, number, number]

export const DEFAULT_ACCENT = '#16746f'
export const ACCENT_STORAGE_KEY = 'insoulforge.accent'
export const ACCENT_PRESETS = [
  {name: '青绿', color: DEFAULT_ACCENT},
  {name: '柔和蓝', color: '#3b6fd8'},
  {name: '靛蓝', color: '#6366d9'},
  {name: '紫色', color: '#8754c9'},
  {name: '玫瑰', color: '#c64f78'},
  {name: '琥珀', color: '#b7791f'}
] as const

export const ACCENT_TOKENS = [
  '--primary', '--primary-strong', '--primary-hover', '--primary-soft', '--primary-softer',
  '--focus-ring', '--selection-bg', '--selection-text', '--neon-cyan-glow'
] as const

export function normalizeAccent(value: string): string | null {
  const hex = value.trim().toLowerCase()
  if (/^#[\da-f]{6}$/.test(hex)) return hex
  if (/^#[\da-f]{3}$/.test(hex)) return '#' + [...hex.slice(1)].map(part => part + part).join('')
  return null
}

export function readAccentColor(storage: Pick<Storage, 'getItem'>): string {
  try { return normalizeAccent(storage.getItem(ACCENT_STORAGE_KEY) ?? '') ?? DEFAULT_ACCENT }
  catch { return DEFAULT_ACCENT }
}

export function saveAccentColor(storage: Pick<Storage, 'setItem' | 'removeItem'>, color: string): boolean {
  try {
    if (color === DEFAULT_ACCENT) storage.removeItem(ACCENT_STORAGE_KEY)
    else storage.setItem(ACCENT_STORAGE_KEY, color)
    return true
  } catch { return false }
}

function rgb(hex: string): RGB {
  const normalized = normalizeAccent(hex)
  if (!normalized) throw new Error('主题颜色必须为十六进制颜色')
  return [1, 3, 5].map(index => parseInt(normalized.slice(index, index + 2), 16)) as unknown as RGB
}
function hex(color: RGB): string {
  return '#' + color.map(channel => Math.round(channel).toString(16).padStart(2, '0')).join('')
}
function mix(from: RGB, to: RGB, amount: number): RGB {
  return from.map((channel, index) => Math.round(channel * (1 - amount) + to[index] * amount)) as unknown as RGB
}
function luminance(color: RGB): number {
  return color.map(channel => {
    const value = channel / 255
    return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4
  }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index]!, 0)
}
function contrast(a: RGB, b: RGB): number {
  const [low, high] = [luminance(a), luminance(b)].sort((x, y) => x - y)
  return (high! + 0.05) / (low! + 0.05)
}
function adjust(base: RGB, toward: RGB, accepts: (color: RGB) => boolean): RGB {
  for (let step = 0; step <= 255; step++) {
    const candidate = mix(base, toward, step / 255)
    if (accepts(candidate)) return candidate
  }
  throw new Error('无法生成具有足够对比度的主题颜色')
}

/** 根据实际主题表面生成强调色，不改变中性色和语义状态色。 */
export function deriveAccentPalette(color: string, mode: ThemeMode, surfaces: readonly string[], card: string,
                                    textColors: readonly string[]): Record<typeof ACCENT_TOKENS[number], string> {
  const seed = rgb(color)
  const backgrounds = surfaces.map(rgb)
  const texts = textColors.map(rgb)
  const white: RGB = [255, 255, 255]
  const black: RGB = [0, 0, 0]
  const tint = (surface: RGB, opacity: number): RGB => surface.map((channel, index) =>
    channel * (1 - opacity) + seed[index]! * opacity) as unknown as RGB
  let alpha = mode === 'dark' ? 0.14 : 0.09
  while (alpha > 0 && backgrounds.some(surface => texts.some(text => contrast(text, tint(surface, alpha)) < 4.52))) {
    alpha = Math.max(0, Math.round((alpha - 0.01) * 100) / 100)
  }
  const tinted = backgrounds.map(surface => tint(surface, alpha))
  const primary = adjust(seed, mode === 'dark' ? white : black,
    candidate => [...backgrounds, ...tinted].every(surface => contrast(candidate, surface) >= 4.6))
  let button = adjust(seed, black, candidate => contrast(white, candidate) >= 4.6)
  if (mode === 'dark') button = adjust(button, white, candidate => contrast(candidate, rgb(card)) >= 3.1)
  const rgba = (opacity: number): string => `rgba(${seed.join(', ')}, ${opacity})`
  return {
    '--primary': hex(primary),
    '--primary-strong': hex(button),
    '--primary-hover': hex(mix(button, black, 0.12)),
    '--primary-soft': rgba(alpha),
    '--primary-softer': rgba(Math.min(alpha, mode === 'dark' ? 0.07 : 0.05)),
    '--focus-ring': hex(primary),
    '--selection-bg': mode === 'dark' ? hex(button) : hex(mix(seed, white, 0.82)),
    '--selection-text': mode === 'dark' ? '#ffffff' : textColors[0]!,
    '--neon-cyan-glow': rgba(mode === 'dark' ? 0.5 : 0.35)
  }
}

export function applyAccentColor(root: HTMLElement, color: string, mode: ThemeMode): void {
  if (color === DEFAULT_ACCENT) {
    for (const token of ACCENT_TOKENS) root.style.removeProperty(token)
    return
  }
  const style = getComputedStyle(root)
  const surfaces = ['--bg-main', '--bg-secondary', '--bg-sidebar', '--card-bg', '--input-bg', '--code-bg']
    .map(token => style.getPropertyValue(token).trim())
  const texts = ['--text-primary', '--text-secondary', '--text-light'].map(token => style.getPropertyValue(token).trim())
  const palette = deriveAccentPalette(color, mode, surfaces, style.getPropertyValue('--card-bg').trim(), texts)
  for (const token of ACCENT_TOKENS) root.style.setProperty(token, palette[token])
}
