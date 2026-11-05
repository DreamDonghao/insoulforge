/// @file MemoryMaintenanceStore.cpp
/// @brief 聊天记忆与好感度结果的事务存储实现

#include <unordered_set>

#include <cstring>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/Transaction.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>

namespace insoulforge::MemoryMaintenanceStore {
    namespace {
        /// @brief 把 f32 向量转换为 SQLite BLOB
        [[nodiscard]] auto toBytes(const std::vector<f32> &embedding) -> std::vector<u8> {
            std::vector<u8> bytes(embedding.size() * sizeof(f32));
            if (!bytes.empty()) {
                std::memcpy(bytes.data(), embedding.data(), bytes.size());
            }
            return bytes;
        }
    } // namespace

    void apply(const Database &database, const SessionId &sessionId, const MemoryMaintenanceResult &result) {
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        Transaction transaction(handle);
        if (result.shortTerm) {
            const Statement statement(handle,
              "INSERT INTO short_term_memory (session_id, memory_content) VALUES (?, ?) "
              "ON CONFLICT(session_id) DO UPDATE SET memory_content = excluded.memory_content, "
              "updated_at = CURRENT_TIMESTAMP");
            statement.bind(1, sessionId.toString());
            statement.bind(2, *result.shortTerm);
            statement.execOrThrow();
        }

        if (!result.longTerm.empty()) {
            const Statement statement(
              handle, "INSERT INTO long_term_memory (session_id, content, embedding) VALUES (?, ?, ?)");
            for (const auto &[content, embedding]: result.longTerm) {
                statement.bind(1, sessionId.toString());
                statement.bind(2, content);
                statement.bind(3, toBytes(embedding));
                statement.execOrThrow();
                statement.reset();
            }
        }
        const std::unordered_set<i64> replacedIds(result.replacedLongTermIds.begin(), result.replacedLongTermIds.end());
        if (!replacedIds.empty()) {
            const Statement statement(handle, "DELETE FROM long_term_memory WHERE id = ? AND session_id = ?");
            for (const i64 memoryId: replacedIds) {
                statement.bind(1, memoryId);
                statement.bind(2, sessionId.toString());
                statement.execOrThrow();
                statement.reset();
            }
        }

        if (!result.personEntries.empty()) {
            const Statement profile(handle, "INSERT OR IGNORE INTO person_memories (user_id) VALUES (?)");
            const Statement entry(
              handle, "INSERT INTO person_memory_entries (user_id, source_session, content) VALUES (?, ?, ?)");
            for (const auto &[userId, content]: result.personEntries) {
                const auto beforeTokens = PersonMemoryStore::contextSnapshot(userId);
                profile.bind(1, userId.toString());
                profile.execOrThrow();
                profile.reset();
                entry.bind(1, userId.toString());
                entry.bind(2, sessionId.toString());
                entry.bind(3, content);
                entry.execOrThrow();
                entry.reset();
                PersonMemoryStore::updateTokenEstimate(userId, beforeTokens);
            }
        }

        if (!result.affinity.empty() && Config::instance().affinityEnabled.load(std::memory_order_relaxed)) {
            const Statement statement(handle,
              "INSERT INTO person_affinity (user_id, affinity) VALUES (?, max(min(?, 100), -100)) "
              "ON CONFLICT(user_id) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
            for (const auto &[userId, delta]: result.affinity) {
                // 批量执行期间仍响应好感度开关的热更新。
                if (!Config::instance().affinityEnabled.load(std::memory_order_relaxed))
                    break;
                statement.bind(1, userId.toString());
                statement.bind(2, delta);
                statement.bind(3, delta);
                statement.execOrThrow();
                statement.reset();
            }
        }
        transaction.commit();
    }
} // namespace insoulforge::MemoryMaintenanceStore
