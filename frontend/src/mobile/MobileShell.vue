<script setup lang="ts">
import {computed, nextTick, onBeforeUnmount, onMounted, provide, ref} from 'vue'
import NavIcon from '../components/NavIcon.vue'
const props = defineProps<{view: string, navigate: (view: string) => boolean}>()
const emit = defineEmits<{theme: [], logout: []}>()
const tabs = [{key:'dashboard',label:'首页',icon:'home'},{key:'groups',label:'会话',icon:'groups'},{key:'people',label:'人物',icon:'admins'},{key:'settings',label:'设置',icon:'execution'}]
const shortcuts = [{key:'logs',label:'运行日志'},{key:'requestDebug',label:'请求调试'},{key:'system-tokens',label:'上下文占用'},{key:'usage',label:'用量统计'},{key:'emojis',label:'表情库'},{key:'activities',label:'实时活动'}]
const settings = [{key:'llm',label:'模型配置'},{key:'executionConfig',label:'执行配置'},{key:'qqConfig',label:'OneBot 配置'},{key:'prompts',label:'提示词'},{key:'memoryConfig',label:'记忆与上下文'},{key:'customTools',label:'工具配置'},{key:'mcpServers',label:'MCP 服务'},{key:'admins',label:'访问管理'},{key:'themeSettings',label:'主题配色'},{key:'about',label:'关于'}]
const routes = [...tabs,...shortcuts,...settings]
const detailActive = ref(false)
let detailBack: (()=>void) | undefined
provide('mobileDetail', (active:boolean) => { detailActive.value=active })
provide('mobileBack', (handler:()=>void) => { detailBack=handler })
const root = computed(()=>!detailActive.value && tabs.some(tab=>tab.key===props.view))
const title = computed(()=>routes.find(route=>route.key===props.view)?.label||'首页')
provide('mobileNavigate', (view:string) => route(view))
provide('mobileViewCache', new Map<string, Record<string, unknown>>())
const positions = new Map<string,number>()
function route(view: string, push = true): void {
  if (!routes.some(route=>route.key===view)) return
  positions.set(props.view,window.scrollY)
  if (!props.navigate(view)) return
  if(push) history.pushState({mobile:true},'',`#mobile/${view}`)
  void nextTick(()=>window.scrollTo(0,positions.get(view)||0))
}
function pop(): void {
  positions.set(props.view,window.scrollY)
  const target=location.hash.match(/^#mobile\/([^/]+)/)?.[1]||'dashboard'
  if (!props.navigate(target)) history.pushState({mobile:true},'',`#mobile/${props.view}`)
  else void nextTick(()=>window.scrollTo(0,positions.get(target)||0))
}
function back(): void { if(detailActive.value){detailBack?.();return} route(settings.some(item=>item.key===props.view)?'settings':'dashboard') }
onMounted(()=>{ const target=location.hash.match(/^#mobile\/([^/]+)/)?.[1];if(target)route(target,false);else history.replaceState({mobile:true},'',`#mobile/${props.view}`);window.addEventListener('popstate',pop) })
onBeforeUnmount(()=>window.removeEventListener('popstate',pop))
</script>
<template>
  <div class="mobile-app">
    <header class="mobile-top"><button v-if="!root" class="mobile-back" aria-label="返回" @click="back">‹</button><h1>{{ title }}</h1></header>
    <main>
      <template v-if="view==='settings'">
        <div class="mobile-menu"><button v-for="item in settings" :key="item.key" @click="route(item.key)">{{ item.label }}<span>›</span></button></div>
        <div class="mobile-menu"><button @click="emit('theme')">切换深浅模式<span>◐</span></button><button class="mobile-danger" @click="emit('logout')">退出登录</button></div>
      </template>
      <template v-else>
        <slot/>
      </template>
    </main>
    <nav v-if="root" class="mobile-bottom" aria-label="主要导航"><button v-for="tab in tabs" :key="tab.key" :aria-current="view===tab.key?'page':undefined" @click="route(tab.key)"><NavIcon :name="tab.icon"/><span>{{ tab.label }}</span></button></nav>
  </div>
</template>
<style scoped>
.mobile-app{font-size:14px;padding-bottom:calc(68px + env(safe-area-inset-bottom));min-width:0}
.mobile-top{position:sticky;top:0;z-index:30;display:flex;align-items:center;gap:4px;height:52px;padding:0 12px;background:var(--bg-sidebar)}
h1{font-size:18px;margin:0}.mobile-back{font-size:30px;width:40px;background:none;border:0;color:inherit;min-height:44px}
main{padding:12px}.mobile-menu{background:var(--card-bg);border-radius:12px;margin-bottom:12px;overflow:hidden}
.mobile-menu button{display:flex;justify-content:space-between;width:100%;text-align:left;padding:10px 12px;min-height:44px;border:0;border-bottom:1px solid var(--border);background:none;color:var(--text-primary);font:inherit}
.mobile-menu span{color:var(--text-secondary)}.mobile-menu .mobile-danger{color:var(--danger)}

.mobile-bottom{position:fixed;bottom:0;left:0;right:0;z-index:35;display:grid;grid-template-columns:repeat(4,1fr);padding:4px 0 calc(4px + env(safe-area-inset-bottom));background:var(--bg-sidebar);border-top:1px solid var(--border)}
.mobile-bottom button{display:grid;justify-items:center;align-content:center;gap:3px;min-height:52px;border:0;background:none;color:var(--text-secondary);font-size:12px}.mobile-bottom button[aria-current]{color:var(--primary);font-weight:600}.mobile-bottom :deep(svg){width:22px;height:22px}
button:focus-visible{outline:2px solid var(--focus-ring);outline-offset:-2px}
</style>
