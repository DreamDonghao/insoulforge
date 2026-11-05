/// <reference types="vite/client" />

export interface QQConfig {
    accessToken: string
    selfQQNumber: string
    oneBotTransport: 'http' | 'websocket'
    qqHttpHost: string
    qqWebSocketHost: string
    botName: string
    defaultPrivateEnabled: boolean
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
    promptCacheKeyEnabled?: boolean
    reasoningEffort: string
    timeoutSeconds?: number
    maxRetries?: number
    minConfidence?: number
    apiType?: 'openai' | 'openrouter'
    editPath?: string
}

export interface MemoryConfig {
    affinityEnabled: boolean
    contextWindowLimit: number
    sharedConversationMessageCount: number
    memorySummaryTriggerCount: number
    memorySummaryBatchSize: number
    memorySummaryContextCount: number
    conversationSummaryMaxChars: number
    conversationSummaryMaxCount: number
    personMemoryEntryMaxChars: number
    longTermMemoryEntryMaxChars: number
    personProfileEntryMaxChars: number
    personProfileMaxChars: number
    personRelationshipEntryMaxChars: number
    personRelationshipMaxChars: number
    personInteractionEntryMaxChars: number
    personInteractionMaxChars: number
    jevWindowSize: number
    routerWindowTriggerCount: number
    routerWindowKeepCount: number
    longTermRecallThreshold: number
    longTermInjectThreshold: number
}

export interface ExecutionConfig {
    maxToolRounds: number
  routeWhileReplying: boolean
  mcpToolPageSize: number
  mcpMaxToolsPerEnable: number
  mcpMaxToolsPerReply: number
}

export interface LongTermMemoryEntry {
    id: number
    sessionId: string
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
    user_id: string
}

export interface BlacklistEntry {
    user_id: string
}

export interface SessionSummary {
    sessionId: string
    sessionType: 'group' | 'private'
    targetId: string
    sessionName: string
    messageCount: number
    sendGroupCard?: boolean
    autoReply?: boolean
    commandEnabled?: boolean
    crossSessionSharing?: boolean
    hasAdditionalPrompt?: boolean
    enabled?: boolean
}

export interface ChatMessage {
    id?: number
    role: 'user' | 'assistant'
    content: string
}

export interface AffinityEntry {
    /** 用户 ID 字符串（大数安全） */
    user_id: string
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
    /** 规范会话 ID，形如 group:123 或 private:some-user */
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
    /** 规范会话 ID，形如 group:123 或 private:some-user，null 表示系统级请求 */
    sessionId?: string | null
    requestBody?: string | null
    responseBody?: string | null
}

export interface HttpTraceListResult {
    entries: HttpTraceEntry[]
    total: number
}
