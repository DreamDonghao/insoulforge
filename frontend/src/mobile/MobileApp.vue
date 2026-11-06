<script setup lang="ts">
import {computed, defineAsyncComponent} from 'vue'
import MobileShell from './MobileShell.vue'
import {transitionalPages} from './transitionalPages'

const MobileHomePage=defineAsyncComponent(()=>import('./pages/DashboardPage.vue'))
const MobilePeoplePage=defineAsyncComponent(()=>import('./pages/PeoplePage.vue'))
const MobileLogsPage=defineAsyncComponent(()=>import('./pages/LogsPage.vue'))
const MobileSessionsPage=defineAsyncComponent(()=>import('./pages/SessionsPage.vue'))
const mobilePages = {dashboard: MobileHomePage, groups: MobileSessionsPage, people: MobilePeoplePage, logs: MobileLogsPage, ...transitionalPages}
const mobilePage = computed(() => mobilePages[currentView.value as keyof typeof mobilePages])

const props=defineProps<{view:string;navigate:(view:string)=>boolean;accent:string;storageError:string}>()
const currentView=computed(()=>props.view)
const emit=defineEmits<{theme:[],logout:[],change:[color:string]}>()
</script>
<template><MobileShell :view="view" :navigate="navigate" @theme="emit('theme')" @logout="emit('logout')"><component :is="mobilePage" v-if="mobilePage" :accent="accent" :storage-error="storageError" @change="emit('change',$event)"/></MobileShell></template>
