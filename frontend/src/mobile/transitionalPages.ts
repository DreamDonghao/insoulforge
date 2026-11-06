// Explicit migration boundary: these screens do not yet have independent mobile views.
import {defineAsyncComponent} from 'vue'
const ThemeSettings = defineAsyncComponent(() => import('../features/appearance/ThemeSettings.vue'))
const About = defineAsyncComponent(() => import('../features/overview/About.vue'))
const ExecutionConfig = defineAsyncComponent(() => import('../features/agent/ExecutionConfig.vue'))
const AccessManager = defineAsyncComponent(() => import('../features/access/AccessManager.vue'))
const MemoryConfig = defineAsyncComponent(() => import('../features/conversation/MemoryConfig.vue'))
const LLMConfig = defineAsyncComponent(() => import('../features/llm/LLMConfig.vue'))
const PromptEditor = defineAsyncComponent(() => import('../features/llm/PromptEditor.vue'))
const SystemTokens = defineAsyncComponent(() => import('../features/llm/SystemTokens.vue'))
const UsageStats = defineAsyncComponent(() => import('../features/llm/UsageStats.vue'))
const OneBotConfig = defineAsyncComponent(() => import('../features/onebot/OneBotConfig.vue'))
const CustomTools = defineAsyncComponent(() => import('../features/tools/CustomTools.vue'))
const McpServers = defineAsyncComponent(() => import('../features/tools/McpServers.vue'))
const Activities = defineAsyncComponent(() => import('../features/tools/Activities.vue'))
const EmojiManager = defineAsyncComponent(() => import('../features/tools/EmojiManager.vue'))
const RequestDebug = defineAsyncComponent(() => import('../features/diagnostics/RequestDebug.vue'))
export const transitionalPages = {requestDebug: RequestDebug, 'system-tokens': SystemTokens, usage: UsageStats, emojis: EmojiManager, activities: Activities, llm: LLMConfig, executionConfig: ExecutionConfig, qqConfig: OneBotConfig, prompts: PromptEditor, memoryConfig: MemoryConfig, customTools: CustomTools, mcpServers: McpServers, admins: AccessManager, themeSettings: ThemeSettings, about: About}
