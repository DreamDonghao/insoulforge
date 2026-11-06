<script setup lang="ts">
import {defineAsyncComponent,toRefs} from 'vue'
import NavIcon from '../components/NavIcon.vue'
const ThemeSettings = defineAsyncComponent(() => import('../features/appearance/ThemeSettings.vue'))
const Dashboard = defineAsyncComponent(() => import('../features/overview/Dashboard.vue'))
const About = defineAsyncComponent(() => import('../features/overview/About.vue'))
const ExecutionConfig = defineAsyncComponent(() => import('../features/agent/ExecutionConfig.vue'))
const AccessManager = defineAsyncComponent(() => import('../features/access/AccessManager.vue'))
const GroupManager = defineAsyncComponent(() => import('../features/conversation/GroupManager.vue'))
const PeopleManager = defineAsyncComponent(() => import('../features/conversation/PeopleManager.vue'))
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
const LogViewer = defineAsyncComponent(() => import('../features/diagnostics/LogViewer.vue'))
const RequestDebug = defineAsyncComponent(() => import('../features/diagnostics/RequestDebug.vue'))
interface Context {currentView:string;dailyNavItems:{key:string;label:string;icon:string}[];systemNavItems:{key:string;label:string;icon:string}[];avatarUrl:string;avatarFailed:boolean;qqConfig:{botName:string};navIndicator:{top:number;height:number};sidebarNav:HTMLElement|null;changeView:(view:string)=>boolean;logoutSubmitting:boolean;mobileNavigationOpen:boolean;isMobile:boolean;logout:()=>void;themeToggleLabel:string;theme:'light'|'dark';toggleTheme:()=>void;resetPageScroll:()=>void;accentColor:string;accentStorageError:string;setAccentColor:(color:string)=>void}
const props=defineProps<{state:Context}>()
const {currentView,dailyNavItems,systemNavItems,avatarUrl,avatarFailed,qqConfig,navIndicator,sidebarNav,changeView,logoutSubmitting,mobileNavigationOpen,isMobile,logout,themeToggleLabel,theme,toggleTheme,resetPageScroll,accentColor,accentStorageError,setAccentColor}=toRefs(props.state)
</script>
<template>
    <!-- 侧边栏 -->
    <div class="sidebar" :style="{'--nav-item-count': dailyNavItems.length + systemNavItems.length + 1}">
      <div class="sidebar-header">
        <div class="sidebar-logo">
          <div class="logo-icon">
            <img v-if="avatarUrl && !avatarFailed" :alt="qqConfig.botName" :src="avatarUrl"
                 @error="avatarFailed = true">
            <svg v-else fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"
                 stroke-width="2" viewBox="0 0 24 24">
              <path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/>
            </svg>
          </div>
          <div>
            <div id="navigation-title" class="sidebar-title">InSoulForge</div>
            <div class="sidebar-subtitle">管理后台</div>
          </div>
        </div>
        <button v-if="isMobile" class="mobile-navigation-close" type="button" aria-label="关闭导航菜单" data-modal-initial-focus @click="mobileNavigationOpen = false">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="m6 6 12 12M6 18 18 6"/></svg>
        </button>
      </div>

      <nav id="app-navigation" aria-label="管理后台导航" ref="sidebarNav" class="sidebar-nav">
        <span class="nav-indicator" aria-hidden="true"
              :style="{height: `${navIndicator.height}px`, transform: `translateY(${navIndicator.top}px)`}"></span>
        <div class="nav-section">
          <button
              v-for="item in dailyNavItems"
              type="button"
              :key="item.key"
              :title="item.label"
              :aria-label="item.label"
              :class="{ active: currentView === item.key }"
              class="nav-item"
              @click="changeView(item.key)"
          >
            <NavIcon :name="item.icon"/>
            {{ item.label }}
          </button>
        </div>

        <div class="nav-divider nav-divider-line"></div>

        <div class="nav-section">
          <button
              v-for="item in systemNavItems"
              type="button"
              :key="item.key"
              :title="item.label"
              :aria-label="item.label"
              :class="{ active: currentView === item.key }"
              class="nav-item"
              @click="changeView(item.key)"
          >
            <NavIcon :name="item.icon"/>
            {{ item.label }}
          </button>
        </div>

        <div class="nav-divider nav-divider-line"></div>

        <div class="nav-section">
          <button
              type="button"
              :class="{ active: currentView === 'about' }"
              title="关于"
              aria-label="关于"
              class="nav-item"
              @click="changeView('about')"
          >
            <NavIcon name="info"/>
            关于
          </button>
        </div>
      </nav>

      <div class="sidebar-footer">
        <div class="sidebar-actions">
          <button class="sidebar-action sidebar-action-danger" type="button" aria-label="退出登录" title="退出登录" :disabled="logoutSubmitting" @click="mobileNavigationOpen = false; logout()">
            <svg fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                 viewBox="0 0 24 24">
              <path d="M10 17l5-5-5-5M15 12H3M21 19V5a2 2 0 0 0-2-2h-6"/>
            </svg>
            <span>退出登录</span>
          </button>
          <button class="sidebar-action" :class="{active: currentView === 'themeSettings'}" :aria-current="currentView === 'themeSettings' ? 'page' : undefined"
                  type="button" aria-label="主题配色" title="主题配色" @click="changeView('themeSettings')">
            <svg fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M12 3a9 9 0 1 0 0 18h1a2 2 0 0 0 1.4-3.4 1 1 0 0 1 .7-1.7H17a4 4 0 0 0 4-4A9 9 0 0 0 12 3Z"/>
              <circle cx="7.5" cy="10" r=".7"/><circle cx="10" cy="6.5" r=".7"/><circle cx="14.5" cy="6.5" r=".7"/><circle cx="17.5" cy="10" r=".7"/>
            </svg>
            <span>主题配色</span>
          </button>
          <button :aria-label="themeToggleLabel" :title="themeToggleLabel" class="sidebar-action" type="button"
                  @click="toggleTheme">
            <svg v-if="theme === 'dark'" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
            </svg>
            <svg v-else fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <circle cx="12" cy="12" r="5"/>
              <line x1="12" x2="12" y1="1" y2="3"/>
              <line x1="12" x2="12" y1="21" y2="23"/>
              <line x1="4.22" x2="5.64" y1="4.22" y2="5.64"/>
              <line x1="18.36" x2="19.78" y1="18.36" y2="19.78"/>
              <line x1="1" x2="3" y1="12" y2="12"/>
              <line x1="21" x2="23" y1="12" y2="12"/>
              <line x1="4.22" x2="5.64" y1="19.78" y2="18.36"/>
              <line x1="18.36" x2="19.78" y1="5.64" y2="4.22"/>
            </svg>
            <span>{{ theme === 'dark' ? '深色模式' : '浅色模式' }}</span>
          </button>
        </div>
      </div>
    </div>

    <!-- 主内容区 -->
    <div class="main">
      <Transition name="page-slide" :duration="240" @before-enter="resetPageScroll">
        <div :key="currentView" class="page-content">
          <Dashboard v-if="currentView === 'dashboard'"/>
          <LLMConfig v-else-if="currentView === 'llm'"/>
          <ExecutionConfig v-else-if="currentView === 'executionConfig'"/>
          <PromptEditor v-else-if="currentView === 'prompts'"/>
          <CustomTools v-else-if="currentView === 'customTools'"/>
          <McpServers v-else-if="currentView === 'mcpServers'"/>
          <Activities v-else-if="currentView === 'activities'"/>
          <EmojiManager v-else-if="currentView === 'emojis'"/>
          <AccessManager v-else-if="currentView === 'admins'"/>
          <GroupManager v-else-if="currentView === 'groups'"/>
          <PeopleManager v-else-if="currentView === 'people'"/>
          <LogViewer v-else-if="currentView === 'logs'"/>
          <RequestDebug v-else-if="currentView === 'requestDebug'"/>
          <MemoryConfig v-else-if="currentView === 'memoryConfig'"/>
          <OneBotConfig v-else-if="currentView === 'qqConfig'"/>
          <SystemTokens v-else-if="currentView === 'system-tokens'"/>
          <UsageStats v-else-if="currentView === 'usage'"/>
          <About v-else-if="currentView === 'about'"/>
          <ThemeSettings v-else-if="currentView === 'themeSettings'" :accent="accentColor" :storage-error="accentStorageError" @change="setAccentColor"/>
        </div>
      </Transition>
    </div>
</template>
