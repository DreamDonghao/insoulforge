export const I32_MAX = 2147483647
export const U64_MAX = 18446744073709551615n
export const SESSION_ID_MAX = (1n << 63n) - 1n

export function identifierError(value: string, label: string, allowZero = false, maximum = U64_MAX): string {
  const text = value.trim()
  if (!(allowZero ? /^(0|[1-9]\d*)$/ : /^[1-9]\d*$/).test(text)) {
    return `请输入${label}，仅支持${allowZero ? '非负' : '正'}整数数字`
  }
  return text.length > 20 || BigInt(text) > maximum ? `${label}超出允许范围` : ''
}

export function numberError(value: unknown, minimum: number, maximum: number, integer = true): string {
  return typeof value !== 'number' || !Number.isFinite(value) || (integer && !Number.isInteger(value)) ||
      value < minimum || value > maximum ? `请输入 ${minimum}～${maximum} 的${integer ? '整数' : '数值'}` : ''
}
