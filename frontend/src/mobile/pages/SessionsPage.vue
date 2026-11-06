<script setup lang="ts">
import {reactive} from 'vue'
import {useSessions} from '../../shared/features/conversation/useSessions'
import SessionDialogs from '../../shared/components/SessionDialogs.vue'
import MobileSessions from '../MobileSessions.vue'
import MobileChat from '../MobileChat.vue'
const state=reactive(useSessions())
</script>
<template>
 <MobileSessions v-if="!state.selectedGroup" :state="state"/>
 <MobileChat v-else :state="state" :bot-name="state.qqConfig?.botName||'InSoulForge'"/>
 <SessionDialogs :state="state"/>
</template>
