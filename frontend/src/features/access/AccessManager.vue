<script lang="ts" setup>
/**
 * @file AccessManager.vue
 * @brief 管理机器人管理员与全局 QQ 黑名单
 */
import {inject, onMounted, reactive, ref, type Ref} from 'vue'
import FieldError from '../../components/FieldError.vue'
import {useFormValidation} from '../../composables/useFormValidation'
import {identifierError} from '../../utils/validation'
import RequestError from '../../components/RequestError.vue'
import {requestJson, requestError} from '../../utils/api'
import type {Admin, ApiResponse, BlacklistEntry} from '../../vite-env'

const showToast = inject<(msg: string, isError?: boolean) => void>('showToast')

const admins: Ref<Admin[]> = ref([])
const blacklist: Ref<BlacklistEntry[]> = ref([])
const newAdminQQ: Ref<string> = ref('')
const newBlacklistQQ: Ref<string> = ref('')
const loading: Ref<boolean> = ref(false)
const saving: Ref<boolean> = ref(false)
const blacklistLoading: Ref<boolean> = ref(false)
const blacklistSaving: Ref<boolean> = ref(false)

const removingAdmins = reactive(new Set<string>())
const removingBlacklist = reactive(new Set<string>())
const adminError = ref('')
const blacklistError = ref('')
const adminsLoaded = ref(false)
const blacklistLoaded = ref(false)

const loadAdmins = async (): Promise<void> => {
  if (loading.value) return
  loading.value = true
  try {
    const data = await requestJson<Admin[]>('/admin/api/admins')
    if (!Array.isArray(data)) throw new Error('管理员列表数据无效')
    admins.value = data.map(item => ({...item, qq: String(item.qq)}))
    adminsLoaded.value = true
    adminError.value = ''
  } catch (cause) {
    adminError.value = requestError(cause, '管理员列表加载失败')
  } finally {
    loading.value = false
  }
}

const loadBlacklist = async (): Promise<void> => {
  if (blacklistLoading.value) return
  blacklistLoading.value = true
  try {
    const data = await requestJson<BlacklistEntry[]>('/admin/api/blacklist')
    if (!Array.isArray(data)) throw new Error('黑名单数据无效')
    blacklist.value = data.map(item => ({...item, qq: String(item.qq)}))
    blacklistLoaded.value = true
    blacklistError.value = ''
  } catch (cause) {
    blacklistError.value = requestError(cause, '黑名单加载失败')
  } finally {
    blacklistLoading.value = false
  }
}

const adminForm = useFormValidation(() => ({'admin-qq': identifierError(newAdminQQ.value, 'QQ 号')}))
const blacklistForm = useFormValidation(() => ({'blacklist-qq': identifierError(newBlacklistQQ.value, 'QQ 号')}))
const addAdmin = async (): Promise<void> => {
  if (saving.value) return
  if (!adminForm.validateForm()) return
  saving.value = true
  try {
    const resp = await fetch('/admin/api/admin', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({qq: newAdminQQ.value.trim()})
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!('管理员已添加')
      newAdminQQ.value = ''
      adminForm.resetValidation()
      await loadAdmins()
    } else {
      showToast!(data.error || '添加失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '添加管理员失败'), true)
  } finally {
    saving.value = false
  }
}

const removeAdmin = async (qq: string): Promise<void> => {
  if (removingAdmins.has(qq)) return
  removingAdmins.add(qq)
  try {
    await requestJson<ApiResponse>(`/admin/api/admin/${qq}`, {method: 'DELETE'})
    showToast!('移除管理员成功')
    await loadAdmins()
  } catch (cause) {
    showToast!(requestError(cause, '移除管理员失败'), true)
  } finally {
    removingAdmins.delete(qq)
  }
}

const addBlacklistEntry = async (): Promise<void> => {
  if (blacklistSaving.value) return
  if (!blacklistForm.validateForm()) return
  blacklistSaving.value = true
  try {
    const resp = await fetch('/admin/api/blacklist', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({qq: newBlacklistQQ.value.trim()})
    })
    const data: ApiResponse = await resp.json()
    if (resp.ok && data.success) {
      showToast!('黑名单已添加')
      newBlacklistQQ.value = ''
      blacklistForm.resetValidation()
      await loadBlacklist()
    } else {
      showToast!(data.error || '添加失败', true)
    }
  } catch (cause) {
    showToast!(requestError(cause, '添加黑名单失败'), true)
  } finally {
    blacklistSaving.value = false
  }
}

const removeBlacklistEntry = async (qq: string): Promise<void> => {
  if (removingBlacklist.has(qq)) return
  removingBlacklist.add(qq)
  try {
    await requestJson<ApiResponse>(`/admin/api/blacklist/${qq}`, {method: 'DELETE'})
    showToast!('移出黑名单成功')
    await loadBlacklist()
  } catch (cause) {
    showToast!(requestError(cause, '移出黑名单失败'), true)
  } finally {
    removingBlacklist.delete(qq)
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
          <form class="access-form-row" novalidate @submit.prevent="addAdmin">
            <div class="form-group access-form-group">
              <label for="admin-qq" class="form-label access-form-label">QQ号</label>
              <input v-model="newAdminQQ" v-bind="adminForm.fieldAttrs('admin-qq')" :disabled="saving" class="form-input access-input" placeholder="输入 QQ 号" type="text" inputmode="numeric" autocomplete="off">
              <FieldError field="admin-qq" :message="adminForm.errors.value['admin-qq']"/>
            </div>
            <button :disabled="saving" class="btn btn-primary access-submit" type="submit">
              {{ saving ? '添加中...' : '添加管理员' }}
            </button>
          </form>
        </div>

        <div class="card">
          <div class="card-header">
            <h3 class="card-title">管理员列表</h3>
          </div>
          <div class="table-container">
            <RequestError :message="adminError" :busy="loading" :stale="adminsLoaded" @retry="loadAdmins"/>
            <p v-if="loading && adminsLoaded" role="status">刷新中…</p>
            <template v-if="loading && !adminsLoaded">
              <div class="empty-state"><p>加载中...</p></div>
            </template>
            <template v-else-if="!adminError && admins.length === 0">
              <div class="empty-state"><p>暂无管理员</p></div>
            </template>
            <template v-else-if="admins.length > 0">
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
                    <button :disabled="removingAdmins.has(admin.qq)" class="btn btn-danger btn-sm" @click="removeAdmin(admin.qq)">{{ removingAdmins.has(admin.qq) ? '移除中…' : '移除' }}</button>
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
          <form class="access-form-row" novalidate @submit.prevent="addBlacklistEntry">
            <div class="form-group access-form-group">
              <label for="blacklist-qq" class="form-label access-form-label">QQ号</label>
              <input v-model="newBlacklistQQ" v-bind="blacklistForm.fieldAttrs('blacklist-qq')" :disabled="blacklistSaving" class="form-input access-input" placeholder="输入 QQ 号" type="text" inputmode="numeric" autocomplete="off">
              <FieldError field="blacklist-qq" :message="blacklistForm.errors.value['blacklist-qq']"/>
            </div>
            <button :disabled="blacklistSaving" class="btn btn-danger access-submit" type="submit">
              {{ blacklistSaving ? '添加中...' : '加入黑名单' }}
            </button>
          </form>
        </div>

        <div class="card">
          <div class="card-header">
            <h3 class="card-title">黑名单</h3>
          </div>
          <div class="table-container">
            <RequestError :message="blacklistError" :busy="blacklistLoading" :stale="blacklistLoaded" @retry="loadBlacklist"/>
            <p v-if="blacklistLoading && blacklistLoaded" role="status">刷新中…</p>
            <template v-if="blacklistLoading && !blacklistLoaded">
              <div class="empty-state"><p>加载中...</p></div>
            </template>
            <template v-else-if="!blacklistError && blacklist.length === 0">
              <div class="empty-state"><p>黑名单为空</p></div>
            </template>
            <template v-else-if="blacklist.length > 0">
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
                    <button :disabled="removingBlacklist.has(entry.qq)" class="btn btn-danger btn-sm" @click="removeBlacklistEntry(entry.qq)">{{ removingBlacklist.has(entry.qq) ? '移除中…' : '移除' }}</button>
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
  grid-template-rows: auto auto;
  gap: 0;
}

.access-grid section {
  display: grid;
  grid-template-rows: subgrid;
  grid-row: 1 / span 2;
  gap: 0;
  min-width: 0;
  padding-right: 28px;
}

.access-grid section + section {
  padding: 0 0 0 28px;
  border-left: 1px solid var(--border);
}

.access-grid .card {
  min-width: 0;
  margin: 0;
}

.access-form-card {
  padding: 24px 0;
  border-top: 0;
}

.access-card-header {
  padding: 0 0 12px;
  margin-bottom: 0;
}

.access-card-title {
  margin-bottom: 0;
  font-size: 16px;
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

  .access-grid section {
    grid-template-rows: auto auto;
    grid-row: auto;
    padding: 0;
  }

  .access-grid section + section {
    padding: 0;
    border-left: 0;
    border-top: 1px solid var(--border);
  }
}

@media (max-width: 440px) {
  .access-form-row { flex-wrap: wrap; }
  .access-form-group { flex-basis: 100%; }
}
</style>
