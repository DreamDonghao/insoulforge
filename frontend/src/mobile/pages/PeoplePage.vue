<script setup lang="ts">
import {reactive} from 'vue'
import {usePeople} from '../../shared/features/conversation/usePeople'
import MobilePeople from '../MobilePeople.vue'
const state=reactive(usePeople())
</script>
<template><MobilePeople :state="state"/></template>
