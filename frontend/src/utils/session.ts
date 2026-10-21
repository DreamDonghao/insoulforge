import type {Group} from '../vite-env'

const PRIVATE_FLAG = 1n << 63n

// Private session IDs exceed Number's safe range; never convert them to Number.
export const sessionKey = (session: Group): string => session.groupIdStr ?? String(session.groupId)
export const isPrivateSession = (session: Group): boolean => session.sessionType === 'private'
export const sessionLabel = (session: Group): string =>
    isPrivateSession(session)
        ? session.groupName ? `${session.groupName} (${session.userId ?? ''})` : `私聊 ${session.userId ?? ''}`
        : session.groupName || `群 ${session.groupId}`

export function privateSessionUserId(sessionId: string): string | null {
  const id = BigInt(sessionId)
  return (id & PRIVATE_FLAG) !== 0n ? String(id & ~PRIVATE_FLAG) : null
}
