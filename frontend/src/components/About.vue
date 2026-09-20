<script lang="ts" setup>
/**
 * @file About.vue
 * @brief 关于页面 - 项目信息、使用约束与交流群
 */
import {inject, onMounted, ref, type Ref} from 'vue'
import QRCode from 'qrcode'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const GROUP_ID = '1097487360'
const GROUP_JOIN_URL = 'https://qm.qq.com/q/4HaHuC0L6g'
const qrDataUrl: Ref<string> = ref('')

onMounted(async () => {
  try {
    qrDataUrl.value = await QRCode.toDataURL(GROUP_JOIN_URL, {
      width: 168,
      margin: 1,
      color: {dark: '#1d222c', light: '#ffffff'}
    })
  } catch {
    // 二维码生成失败时静默降级为纯文字群号
  }
})

const copyGroupId = async (): Promise<void> => {
  try {
    await navigator.clipboard.writeText(GROUP_ID)
    showToast?.('群号已复制')
  } catch {
    showToast?.('复制失败，请手动复制', true)
  }
}

const copyJoinUrl = async (): Promise<void> => {
  try {
    await navigator.clipboard.writeText(GROUP_JOIN_URL)
    showToast?.('加群链接已复制')
  } catch {
    showToast?.('复制失败，请手动复制', true)
  }
}
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">关于</h1>
      <p class="page-subtitle">InSoulForge 项目信息</p>
    </div>

    <!-- 项目简介 -->
    <div class="card about-hero">
      <div class="hero-head">
        <span class="hero-title">InSoulForge</span>
        <span class="version-badge">v1.3.0</span>
      </div>
      <p class="hero-desc">一个智能 QQ 群聊机器人，基于两层 Agent 架构，支持自定义角色、长期记忆、多工具调用。</p>
    </div>

    <!-- 项目信息 -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">项目信息</h3>
      </div>
      <div class="info-grid">
        <div class="info-item">
          <span class="info-label">版本</span>
          <span>1.2.1</span>
        </div>
        <div class="info-item">
          <span class="info-label">作者</span>
          <span>DreamDonghao</span>
        </div>
        <div class="info-item">
          <span class="info-label">许可证</span>
          <a href="https://github.com/DreamDonghao/insoulforge/blob/main/LICENSE" target="_blank">AGPL-3.0-only</a>
        </div>
        <div class="info-item">
          <span class="info-label">Github仓库</span>
          <a href="https://github.com/DreamDonghao/insoulforge" target="_blank">DreamDonghao/insoulforge</a>
        </div>
      </div>
    </div>

    <!-- 开源协议 -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">开源协议</h3>
      </div>
      <ul class="constraint-list">
        <li>本项目采用 GNU Affero General Public License v3.0。</li>
        <li>修改并通过网络向用户提供服务时，须依照协议提供对应源代码。</li>
        <li>完整条款以仓库根目录的 LICENSE 文件为准。</li>
      </ul>
    </div>

    <!-- 交流群 -->
    <div class="card">
      <div class="card-header">
        <h3 class="card-title">交流群</h3>
      </div>
      <div class="qq-row">
        <img v-if="qrDataUrl" :alt="'QQ群 ' + GROUP_ID" :src="qrDataUrl" class="qr-img">
        <div class="qq-info">
          <div class="qq-name">InSoulForge 官方交流群</div>
          <code class="qq-id">{{ GROUP_ID }}</code>
          <div class="qq-actions">
            <button class="btn btn-primary btn-sm" type="button" @click="copyGroupId">复制群号</button>
            <button class="btn btn-secondary btn-sm" type="button" @click="copyJoinUrl">复制加群链接</button>
          </div>
          <a :href="GROUP_JOIN_URL" class="join-link" target="_blank">或点击这里直接加群 →</a>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.about-hero {
  background: linear-gradient(135deg, var(--primary-soft), var(--card-bg));
  border-color: var(--neon-cyan-glow);
}

.hero-head {
  display: flex;
  align-items: center;
  gap: 12px;
}

.hero-title {
  font-size: 22px;
  font-weight: 700;
  letter-spacing: -0.4px;
  background: linear-gradient(120deg, var(--primary), var(--neon-cyan));
  -webkit-background-clip: text;
  background-clip: text;
  -webkit-text-fill-color: transparent;
  color: transparent;
}

.version-badge {
  font-size: 12px;
  font-weight: 600;
  color: var(--primary);
  background: var(--primary-soft);
  border: 1px solid var(--focus-ring);
  padding: 2px 10px;
  border-radius: 999px;
}

.hero-desc {
  margin-top: 10px;
  color: var(--text-secondary);
  line-height: 1.6;
}

.info-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 16px;
}

.info-item {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.info-label {
  font-size: 12px;
  color: var(--text-light);
}

.constraint-list {
  margin: 0;
  padding-left: 20px;
  color: var(--text-secondary);
  line-height: 2;
}

.qq-row {
  display: flex;
  align-items: center;
  gap: 24px;
}

.qr-img {
  width: 168px;
  height: 168px;
  border-radius: 8px;
  background: #ffffff;
  padding: 8px;
  border: 1px solid var(--border);
  flex-shrink: 0;
}

.qq-info {
  display: flex;
  flex-direction: column;
  gap: 10px;
  align-items: flex-start;
}

.qq-name {
  font-weight: 600;
  color: var(--text-primary);
}

.qq-id {
  font-size: 16px;
}

.qq-actions {
  display: flex;
  gap: 8px;
}

.join-link {
  font-size: 13px;
}
</style>
