<script setup lang="ts">
import {reactive} from 'vue'
import {useLogs} from '../../shared/features/diagnostics/useLogs'
import MobileLogs from '../MobileLogs.vue'
const state=reactive(useLogs())
</script>
<template><MobileLogs :state="state"/></template>
