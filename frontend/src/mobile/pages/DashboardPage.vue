<script setup lang="ts">
import {reactive} from 'vue'
import {useDashboard} from '../../shared/features/overview/useDashboard'
import MobileDashboard from '../MobileDashboard.vue'
const state=reactive(useDashboard())
</script>
<template><MobileDashboard :state="state" :bot-name="state.qqConfig?.botName||'InSoulForge'"/></template>
