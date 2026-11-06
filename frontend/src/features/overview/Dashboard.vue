<script setup lang="ts">
import {computed, inject, onMounted, onUnmounted, reactive, ref, type Ref} from 'vue'
import type {LLMConfig, QQConfig} from '../../vite-env'

import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import ProcessMemory from './ProcessMemory.vue'
import {useDashboard} from '../../shared/features/overview/useDashboard'
const state = useDashboard()
const {qqConfig, showToast, dataKeys, dataState, dataRequests, disposed, loadData, readData, botRunning, botStatusSaving, oneBotStatus, loadBotStatus, loadOneBotStatus, toggleBotStatus, llmOrder, llmLabels, llmModels, llmLoading, startTimeEpoch, baseUptime, loadMoment, nowTick, uptimeTimer, oneBotStatusTimer, storageTimer, storage, formatStorage, todayStats, todayByRole, byDay, groupCount, enabledCount, totalMessages, adminCount, emojiCount, toolCount, transportLabel, oneBotAddressLabel, oneBotConnectionReady, oneBotConnectionText, fmtNum, fmtUptime, fmtDateTime, uptimeText, todayHitRate, maxDayTotal, loadLLMModels, loadSystemInfo, loadUsage, loadGroups, loadAdmins, loadEmojis, loadTools, dataSections} = state
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">首页</h1>
      <p class="page-subtitle">{{ qqConfig?.botName || 'InSoulForge' }} 运行概况</p>
    </div>

    <div v-for="section in dataSections" :key="section.key">
      <RequestError :message="dataState[section.key].error ? section.label + '：' + dataState[section.key].error : ''"
                    :busy="dataState[section.key].loading" :stale="dataState[section.key].loaded" @retry="section.retry"/>
    </div>
    <p v-if="dataKeys.some(key => dataState[key].loading && !dataState[key].loaded)" role="status">正在加载运行概况…</p>
    <!-- 状态条 -->
    <div class="dash-status-bar">
      <div class="status-item bot-control-item">
        <span :class="dataState.botStatus.loaded && botRunning ? 'dot-green' : 'dot-gray'" class="status-dot"></span>
        <span>{{ !dataState.botStatus.loaded ? '机器人状态待获取' : botRunning ? '机器人运行中' : '机器人已暂停' }}</span>
        <button
            :class="botRunning ? 'btn-warning' : 'btn-success'"
            :disabled="botStatusSaving || !dataState.botStatus.loaded"
            class="btn btn-sm bot-toggle"
            type="button"
            @click="toggleBotStatus"
        >
          {{ botStatusSaving ? '处理中...' : (botRunning ? '暂停机器人' : '打开机器人') }}
        </button>
      </div>
      <div class="status-item">
        <span
            :class="oneBotConnectionReady ? 'dot-green' : oneBotStatus.transport === 'websocket' ? 'dot-red' : 'dot-gray'"
            class="status-dot"></span>
        <span>{{ dataState.oneBotStatus.loaded ? oneBotConnectionText : 'OneBot 状态待获取' }}</span>
      </div>
    </div>

    <ProcessMemory>
      <template #before-metrics>
        <div :title="`启动于 ${fmtDateTime(startTimeEpoch)}`"><dt>运行时间</dt><dd>{{ dataState.system.loaded ? uptimeText : '—' }}</dd></div>
      </template>
      <template #after-metrics>
        <div title="数据库主文件与 WAL、SHM 辅助文件的总大小"><dt>数据库占用</dt><dd>{{ storage ? formatStorage(storage.databaseBytes) : '—' }}</dd></div>
        <div title="整个日志目录，包含子目录和历史日志"><dt>日志占用</dt><dd>{{ storage ? formatStorage(storage.logBytes) : '—' }}</dd></div>
      </template>
      <template #metrics-note>
        <span class="storage-note">启动于 {{ fmtDateTime(startTimeEpoch) }}</span>
        <span class="storage-note">{{ storage ? `存储 ${new Date(storage.timestamp).toLocaleTimeString()} · 每分钟更新` : '存储加载中…' }}</span>
      </template>
    </ProcessMemory>

    <div class="dash-layout">
      <div class="dash-col">
        <section class="overview-section" aria-label="OneBot 配置">
          <h2 class="section-title">OneBot 配置</h2>
          <div class="kv-list">
            <div class="kv-row">
              <span class="kv-label">Bot 名称</span>
              <span class="kv-value">{{ qqConfig?.botName || '—' }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">用户 ID</span>
              <code class="kv-code">{{ qqConfig?.selfQQNumber && qqConfig.selfQQNumber !== '0' ? qqConfig.selfQQNumber : '—' }}</code>
            </div>
            <div class="kv-row">
              <span class="kv-label">传输方式</span>
              <span class="kv-value">{{ dataState.oneBotStatus.loaded ? transportLabel : '—' }}</span>
            </div>
            <div class="kv-row">
              <span class="kv-label">连接状态</span>
              <span :class="oneBotConnectionReady ? 'connection-ready' : 'connection-offline'" class="connection-state">
                {{ !dataState.oneBotStatus.loaded ? '状态待获取' : oneBotConnectionReady ? '已连接' : oneBotStatus.transport === 'http' ? '未配置' : '未连接' }}
              </span>
            </div>
            <div class="kv-row">
              <span class="kv-label">{{ oneBotAddressLabel }}</span>
              <code class="kv-code kv-wrap">{{ oneBotStatus.address || '—' }}</code>
            </div>
          </div>
        </section>
        <section class="overview-section" aria-label="资源">
          <h2 class="section-title">资源</h2>
          <div class="resource-grid">
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.groups.loaded ? enabledCount : '—' }}<span class="resource-total">/{{ dataState.groups.loaded ? groupCount : '—' }}</span></div>
              <div class="resource-label">会话（启用/总）</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.groups.loaded ? fmtNum(totalMessages) : '—' }}</div>
              <div class="resource-label">消息</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.admins.loaded ? adminCount : '—' }}</div>
              <div class="resource-label">管理员</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.emojis.loaded ? emojiCount : '—' }}</div>
              <div class="resource-label">表情</div>
            </div>
            <div class="resource-tile">
              <div class="resource-value">{{ dataState.tools.loaded ? toolCount : '—' }}</div>
              <div class="resource-label">工具</div>
            </div>
          </div>
        </section>
        <section class="overview-section" aria-label="每日用量">
          <div class="section-header">
            <h2 class="section-title">每日用量</h2>
            <span class="section-sub">今日</span>
          </div>

          <div v-if="dataState.usage.loaded" class="usage-kpis">
            <div class="usage-kpi">
              <div class="kpi-label">调用</div>
              <div class="kpi-value">{{ fmtNum(todayStats.calls) }} <span class="kpi-unit">次</span></div>
            </div>
            <div class="usage-kpi">
              <div class="kpi-label">Token</div>
              <div class="kpi-value">{{ fmtNum(todayStats.total) }}</div>
            </div>
            <div class="usage-kpi">
              <div class="kpi-label">缓存命中率</div>
              <div class="kpi-value">{{ todayHitRate.toFixed(1) }}%</div>
            </div>
          </div>

          <div v-if="dataState.usage.loaded" class="usage-chart">
            <div v-for="d in byDay" :key="d.day" :title="`${d.day}: ${fmtNum(d.total)} Token`" class="usage-col">
              <div class="usage-bar-track">
                <div :style="{ height: (d.total / maxDayTotal) * 100 + '%' }" class="usage-bar"></div>
              </div>
              <span class="usage-day">{{ d.day.slice(5) }}</span>
            </div>
          </div>
        </section>
      </div>
      <div class="dash-col">
        <section class="overview-section" aria-label="模型配置">
          <h2 class="section-title">模型配置</h2>
          <div class="models-list">
            <template v-if="llmLoading">
              <span class="section-muted">加载中...</span>
            </template>
            <template v-else-if="!dataState.models.loaded"><span class="section-muted">模型数据待获取</span></template>
            <template v-else-if="llmOrder.every(n => !llmModels[n])">
              <span class="section-muted">未配置</span>
            </template>
            <template v-else>
              <div v-for="name in llmOrder" :key="name" class="model-row">
                <span class="model-name">{{ llmLabels[name] }}</span>
                <code class="model-code">{{ llmModels[name] || '未配置' }}</code>
              </div>
            </template>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
.storage-note { font-size: 12px; color: var(--text-secondary); }
/* ====== 状态条 ====== */
.dash-status-bar {
  display: flex;
  gap: 16px 24px;
  padding: 16px 24px;
  border-bottom: 0;
  flex-wrap: wrap;
  background: var(--card-bg);
  margin-bottom: 24px;
  border-radius: var(--radius-md);
}

.status-item {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--text-secondary);
}

.bot-control-item {
  padding-right: 24px;
  border-right: 0;
}

.bot-toggle {
  margin-left: 4px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot-green {
  background: var(--success);
}

.dot-red {
  background: var(--danger);
}

.dot-gray {
  background: var(--text-light);
}

.dash-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  border-block: 0;
  gap: 24px;
}
.dash-col { min-width: 0; padding: 0; }
.dash-col + .dash-col { border-left: 0; padding: 0; border-top: 0; }

.overview-section {
  min-width: 0;
  flex: none;
  padding: 24px;
  display: flex;
  flex-direction: column;
  background: var(--card-bg);
  margin-bottom: 24px;
  border-radius: var(--radius-md);
}

.overview-section + .overview-section {
  border-top: 0;
}

.section-header {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 16px;
}

.section-header .section-title {
  margin-bottom: 0;
}

.section-title {
  margin: 0 0 16px;
  font-weight: 600;
  font-size: 16px;
  color: var(--text-primary);
}

.section-sub {
  margin-left: auto;
  font-size: 12px;
  color: var(--text-light);
}

.section-muted {
  font-size: 13px;
  color: var(--text-light);
}

/* ====== 模型配置 ====== */
.models-list {
  display: flex;
  flex-direction: column;
  gap: 0;
}

.model-row {
  display: grid;
  grid-template-columns: 96px minmax(0, 1fr);
  align-items: center;
  gap: 10px;
  padding: 18px 0;
  border-bottom: 1px solid var(--border);
}

.model-row:last-child {
  border-bottom: 0;
}

.model-name {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-secondary);
  flex-shrink: 0;
}

.model-code {
  font-size: 14px;
  min-width: 0;
  width: fit-content;
  max-width: 100%;
  overflow-wrap: anywhere;
  white-space: normal;
  padding: 0;
  background: transparent;
}

/* ====== 每日用量 ====== */
.usage-kpis {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
  margin-bottom: 16px;
}

.usage-kpi {
  display: flex;
  flex-direction: column;
}

.kpi-label {
  font-size: 12px;
  color: var(--text-light);
  text-transform: uppercase;
  letter-spacing: 0;
}

.kpi-value {
  overflow-wrap: anywhere;
  font-size: 20px;
  font-weight: 700;
  color: var(--text-primary);
  line-height: 1.25;
  font-variant-numeric: tabular-nums;
}

.kpi-unit {
  font-size: 13px;
  font-weight: 500;
  color: var(--text-secondary);
}

.usage-chart {
  display: flex;
  align-items: flex-end;
  gap: 8px;
  height: 90px;
  min-height: 90px;
}

.usage-col {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  height: 100%;
  justify-content: flex-end;
}

.usage-bar-track {
  width: 100%;
  max-width: 28px;
  height: 70px;
  background: var(--border-soft, var(--bg-secondary));
  border-radius: var(--radius-xs);
  display: flex;
  align-items: flex-end;
  overflow: hidden;
}

.usage-bar {
  width: 100%;
  background: var(--primary-strong);
  border-radius: var(--radius-xs);
  min-height: 2px;
  transition: height 0.3s ease;
}

.usage-day {
  font-size: 10px;
  color: var(--text-light);
  white-space: nowrap;
}

/* ====== kv 列表（OneBot） ====== */
.kv-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.kv-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  min-width: 0;
}

.kv-label {
  font-size: 13px;
  color: var(--text-secondary);
  white-space: nowrap;
}

.kv-value {
  min-width: 0;
  overflow-wrap: anywhere;
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.connection-state {
  font-size: 13px;
  font-weight: 600;
}

.connection-ready {
  color: var(--success);
}

.connection-offline {
  color: var(--text-secondary);
}

.kv-code {
  font-size: 12px;
  max-width: 160px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  padding: 0;
  background: transparent;
}

.kv-wrap {
  min-width: 0;
  max-width: none;
  white-space: normal;
  word-break: break-all;
  text-align: right;
}

/* ====== 资源 ====== */
.resource-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 16px;
}

.resource-tile {
  display: flex;
  flex-direction: column;
  justify-content: center;
  padding: 8px 0;
  min-width: 0;
}

.resource-value {
  overflow-wrap: anywhere;
  font-size: 20px;
  font-weight: 700;
  color: var(--text-primary);
  font-variant-numeric: tabular-nums;
}

.resource-total {
  font-size: 13px;
  font-weight: 500;
  color: var(--text-light);
}

.resource-label {
  font-size: 11px;
  color: var(--text-light);
  margin-top: 2px;
}

/* ====== 响应式 ====== */
@media (max-width: 1100px) {
  .dash-layout {
    grid-template-columns: 1fr;
    grid-template-rows: auto;
  }

  .dash-col {
    display: flex;
    flex-direction: column;
    grid-row: auto;
    padding: 0;
  }

  .dash-col + .dash-col {
    border-left: 0;
    border-top: 0;
    padding: 0;
  }
}

@media (max-width: 640px) {
  .resource-grid {
    grid-template-columns: repeat(2, 1fr);
  }

  .model-row {
    grid-template-columns: 80px minmax(0, 1fr);
    gap: 8px;
  }

  .bot-control-item {
    border-right: 0;
    padding-right: 0;
    flex-wrap: wrap;
  }
}
</style>
