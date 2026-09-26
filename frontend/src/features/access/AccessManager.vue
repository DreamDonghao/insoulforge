<script lang="ts" setup>
/**
 * @file AccessManager.vue
 * @brief 管理机器人管理员与全局 QQ 黑名单
 */
import {inject, onMounted, ref, type Ref} from 'vue'
import type {Admin, ApiResponse, BlacklistEntry} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const admins: Ref<Admin[]> = ref([])
const blacklist: Ref<BlacklistEntry[]> = ref([])
const newAdminQQ: Ref<number | undefined> = ref(undefined)
const newBlacklistQQ: Ref<number | undefined> = ref(undefined)
const loading: Ref<boolean> = ref(false)
const saving: Ref<boolean> = ref(false)
const blacklistLoading: Ref<boolean> = ref(false)
const blacklistSaving: Ref<boolean> = ref(false)

const loadAdmins = async (): Promise<void> => {
  loading.value = true
  try {
    const resp = await fetch('/admin/api/admins')
    if (!resp.ok) {
      showToast!('加载失败: ' + resp.status, true)
      admins.value = []
      return
    }
    const data = await resp.json()
    if (Array.isArray(data)) {
      admins.value = data
    } else {
      admins.value = []
    }
  } catch {
    showToast!('网络错误，请检查后端服务', true)
    admins.value = []
  } finally {
    loading.value = false
  }
}

const loadBlacklist = async (): Promise<void> => {
  blacklistLoading.value = true
  try {
    const resp = await fetch('/admin/api/blacklist')
    if (!resp.ok) {
      showToast!('黑名单加载失败: ' + resp.status, true)
      blacklist.value = []
      return
    }
    const data = await resp.json()
    blacklist.value = Array.isArray(data) ? data : []
  } catch {
    showToast!('黑名单网络错误', true)
    blacklist.value = []
  } finally {
    blacklistLoading.value = false
  }
}

const addAdmin = async (): Promise<void> => {
  if (!newAdminQQ.value) {
    showToast!('请输入QQ号', true)
    return
  }
  saving.value = true
  try {
    const resp = await fetch('/admin/api/admin', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({qq: newAdminQQ.value})
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('管理员已添加')
      newAdminQQ.value = undefined
      await loadAdmins()
    } else {
      showToast!(data.error || '添加失败', true)
    }
  } finally {
    saving.value = false
  }
}

const removeAdmin = async (qq: number): Promise<void> => {
  const resp = await fetch(`/admin/api/admin/${qq}`, {method: 'DELETE'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('管理员已删除')
    await loadAdmins()
  }
}

const addBlacklistEntry = async (): Promise<void> => {
  if (!newBlacklistQQ.value) {
    showToast!('请输入QQ号', true)
    return
  }
  blacklistSaving.value = true
  try {
    const resp = await fetch('/admin/api/blacklist', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({qq: newBlacklistQQ.value})
    })
    const data: ApiResponse = await resp.json()
    if (data.success) {
      showToast!('黑名单已添加')
      newBlacklistQQ.value = undefined
      await loadBlacklist()
    } else {
      showToast!(data.error || '添加失败', true)
    }
  } finally {
    blacklistSaving.value = false
  }
}

const removeBlacklistEntry = async (qq: number): Promise<void> => {
  const resp = await fetch(`/admin/api/blacklist/${qq}`, {method: 'DELETE'})
  const data: ApiResponse = await resp.json()
  if (data.success) {
    showToast!('黑名单已移除')
    await loadBlacklist()
  } else {
    showToast!(data.error || '移除失败', true)
  }
}

onMounted(() => void Promise.all([loadAdmins(), loadBlacklist()]))
</script>

<template>
  <div>
    <div class="page-header">
      <h1 class="page-title">访问管理</h1>
      <p class="page-subtitle">管理机器人管理员与全局 QQ 黑名单</p>
    </div>

    <div class="access-grid">
      <section>
        <div class="card access-form-card">
          <div class="card-header access-card-header">
            <h3 class="card-title access-card-title">添加管理员</h3>
          </div>
          <div class="access-form-row">
            <div class="form-group access-form-group">
              <label class="form-label access-form-label">QQ号</label>
              <input v-model.number="newAdminQQ" class="form-input access-input" placeholder="输入QQ号" type="number">
            </div>
            <button :disabled="saving" class="btn btn-success access-submit" @click="addAdmin">
              {{ saving ? '添加中...' : '添加管理员' }}
            </button>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <h3 class="card-title">管理员列表</h3>
          </div>
          <div class="table-container">
            <template v-if="loading">
              <div class="empty-state"><p>加载中...</p></div>
            </template>
            <template v-else-if="admins.length === 0">
              <div class="empty-state"><p>暂无管理员</p></div>
            </template>
            <template v-else>
              <table>
                <thead>
                <tr>
                  <th>QQ号</th>
                  <th class="action-column">操作</th>
                </tr>
                </thead>
                <tbody>
                <tr v-for="admin in admins" :key="admin.qq">
                  <td><code>{{ admin.qq }}</code></td>
                  <td>
                    <button class="btn btn-danger btn-sm" @click="removeAdmin(admin.qq)">移除</button>
                  </td>
                </tr>
                </tbody>
              </table>
            </template>
          </div>
        </div>
      </section>

      <section>
        <div class="card access-form-card">
          <div class="card-header access-card-header">
            <h3 class="card-title access-card-title">添加黑名单</h3>
          </div>
          <div class="access-form-row">
            <div class="form-group access-form-group">
              <label class="form-label access-form-label">QQ号</label>
              <input v-model.number="newBlacklistQQ" class="form-input access-input" placeholder="输入QQ号"
                     type="number">
            </div>
            <button :disabled="blacklistSaving" class="btn btn-danger access-submit" @click="addBlacklistEntry">
              {{ blacklistSaving ? '添加中...' : '加入黑名单' }}
            </button>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <h3 class="card-title">黑名单</h3>
          </div>
          <div class="table-container">
            <template v-if="blacklistLoading">
              <div class="empty-state"><p>加载中...</p></div>
            </template>
            <template v-else-if="blacklist.length === 0">
              <div class="empty-state"><p>黑名单为空</p></div>
            </template>
            <template v-else>
              <table>
                <thead>
                <tr>
                  <th>QQ号</th>
                  <th class="action-column">操作</th>
                </tr>
                </thead>
                <tbody>
                <tr v-for="entry in blacklist" :key="entry.qq">
                  <td><code>{{ entry.qq }}</code></td>
                  <td>
                    <button class="btn btn-danger btn-sm" @click="removeBlacklistEntry(entry.qq)">移除</button>
                  </td>
                </tr>
                </tbody>
              </table>
            </template>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.access-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: start;
  gap: 16px;
}

.access-grid section {
  min-width: 0;
  align-self: start;
}

.access-grid .card + .card {
  margin-top: 16px;
}

.access-form-card {
  padding: 12px 16px;
}

.access-card-header {
  padding: 0 0 12px;
  margin-bottom: 0;
}

.access-card-title {
  margin-bottom: 0;
  font-size: 15px;
}

.access-form-row {
  display: flex;
  align-items: flex-end;
  gap: 12px;
}

.access-form-group {
  flex: 1;
  min-width: 0;
  margin: 0;
}

.access-form-label {
  margin-bottom: 4px;
}

.access-input,
.access-submit {
  height: 36px;
}

.access-input {
  padding: 0 8px;
  font-size: 13px;
}

.access-submit {
  flex: 0 0 auto;
  padding: 0 16px;
  line-height: 36px;
}

.action-column {
  width: 100px;
}

@media (max-width: 900px) {
  .access-grid {
    grid-template-columns: 1fr;
  }
}
</style>
