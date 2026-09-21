/// <reference types="vite/client" />

export interface QQConfig {
    accessToken: string
    selfQQNumber: number
    oneBotTransport: 'http' | 'websocket'
    qqHttpHost: string
    qqWebSocketHost: string
    botName: string
}

export interface LLMConfig {
    name?: string
    apiKey: string
    baseUrl: string
    path: string
    model: string
    maxTokens: number
    temperature: number
    topP: number
    reasoningEffort: string
}

export interface MemoryConfig {
    contextWindowLimit: number
    memorySummaryTriggerCount: number
    memorySummaryBatchSize: number
    memorySummaryContextCount: number
    memoryExtractMaxTokens: number
    routerWindowTriggerCount: number
    routerWindowKeepCount: number
    shortTermMemoryMax: number
    longTermRecallThreshold: number
    longTermInjectThreshold: number
}

export interface LongTermMemoryEntry {
    id: number
    groupId: string
    content: string
    createdAt: string
}

export interface LongTermMemoryListResult {
    items: LongTermMemoryEntry[]
    total: number
}

export interface Emoji {
    name: string
    summary: string
    emoji_id: string
    emoji_package_id: string
    key: string
    url: string
    md5: string
    res_id: string
    is_mark_face: boolean
}

export interface Admin {
    qq: number
}

export interface BlacklistEntry {
    qq: number
}

export interface Group {
    groupId: number
    /** 会话 ID 的字符串形式（私聊会话 ID 带标志位，超过 Number 安全范围） */
    groupIdStr?: string
    groupName: string
    messageCount: number
    enabled?: boolean
    sessionType?: 'private'
    userId?: number
}

export interface ChatMessage {
    id?: number
    role: 'user' | 'assistant'
    content: string
}

export interface AffinityEntry {
    /** QQ 号字符串（大数安全） */
    qq: string
    /** 昵称（运行时映射缺失且 OneBot 查询失败时缺省） */
    name?: string
    affinity: number
}

export interface ScheduledTask {
    id: number
    /** 触发时间（unix 秒） */
    remindTime: number
    content: string
    /** 是否每日重复任务 */
    daily: boolean
}

export interface ApiResponse {
    success: boolean
    error?: string
    data?: any
}

export interface LogEntry {
    id: number
    timestamp: string
    level: string
    /** 会话 ID 的字符串形式（私聊会话 ID 带标志位） */
    sessionId: string
    /** 日志来源，例如 Executor、OneBot 或 Config */
    source: string
    /** 由业务模块提供的完整日志内容 */
    content: string
}

export interface LogQueryResult {
    entries: LogEntry[]
    hasMore: boolean
    nextAfterId: number
    nextBeforeId: number
    oldestId: number
    newestId: number
    size: number
    currentLevel: string
}

export interface HttpTraceEntry {
    id: number
    timestamp: string
    tag: string
    method: string
    url: string
    /** 0 表示请求未得到响应（超时/异常） */
    status: number
    /** 会话 ID 的字符串形式（私聊会话 ID 带标志位），null 表示系统级请求 */
    groupId?: string | null
    requestBody?: string | null
    responseBody?: string | null
}

export interface HttpTraceListResult {
    entries: HttpTraceEntry[]
    total: number
}
