// 不把响应正文直接拼进错误提示，避免 HTML 错误页或凭据意外泄露。
export async function requestJson<T>(url: string, options?: RequestInit): Promise<T> {
  const response = await fetch(url, options)
  const data = await response.json().catch(() => { throw new Error(`服务返回了无效数据（HTTP ${response.status}）`) })
  if (!response.ok || data?.success === false) {
    throw new Error(typeof data?.error === 'string' ? data.error : `请求失败（HTTP ${response.status}）`)
  }
  return data as T
}

export function requestError(cause: unknown, fallback: string): string {
  return cause instanceof Error && cause.name !== 'TypeError' ? `${fallback}：${cause.message}` : `${fallback}，请检查网络与后端服务`
}
