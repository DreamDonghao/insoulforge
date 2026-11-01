import type {SessionSummary} from '../vite-env'

export const sessionKey = (session: SessionSummary): string => session.sessionId
export const isPrivateSession = (session: SessionSummary): boolean => session.sessionType === 'private'
export const sessionLabel = (session: SessionSummary): string =>
  isPrivateSession(session)
    ? session.sessionName ? `${session.sessionName} (${session.targetId})` : `私聊 ${session.targetId}`
    : session.sessionName || `群 ${session.targetId}`

export function parseSessionId(value: string): {type: 'private' | 'group', targetId: string} | null {
  const separator = value.indexOf(':')
  const type = value.slice(0, separator)
  if (separator < 0 || (type !== 'private' && type !== 'group')) return null
  const encoded = value.slice(separator + 1)
  if (!encoded || /[^A-Za-z0-9._~%\-]/.test(encoded) || /%(?![0-9A-F]{2})/.test(encoded)) return null
  try {
    const targetId = decodeURIComponent(encoded)
    return targetId && !targetId.includes('\0') && encodeTarget(targetId) === encoded ? {type, targetId} : null
  } catch { return null }
}
const encodeTarget = (value: string): string => encodeURIComponent(value).replace(/[!'()*]/g,
  char => `%${char.charCodeAt(0).toString(16).toUpperCase()}`)
export const privateSessionUserId = (value: string): string | null => {
  const session = parseSessionId(value)
  return session?.type === 'private' ? session.targetId : null
}
export const sessionIdLabel = (value: string | null | undefined): string => {
  if (!value) return '系统'
  const session = parseSessionId(value)
  return session ? `${session.type === 'private' ? '私聊' : '群'} ${session.targetId}` : value
}
