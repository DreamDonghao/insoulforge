/// @file MemoryMaintenanceStore.cpp
/// @brief 聊天记忆与好感度结果的事务存储实现

#include <unordered_set>

#include <cstring>

#include <agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

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

        void execOrThrow(sqlite3 *database, const char *sql) {
            char *error = nullptr;
            if (sqlite3_exec(database, sql, nullptr, nullptr, &error) == SQLITE_OK) {
                return;
            }
            const std::string message = error ? error : sqlite3_errmsg(database);
            sqlite3_free(error);
            throw DbError(message);
        }
    } // namespace

    void apply(const Database &database, const u64 sessionId, const MemoryMaintenanceResult &result,
      const AffinityDeltas &affinity) {
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        execOrThrow(handle, "BEGIN IMMEDIATE");
        try {
            if (result.shortTerm) {
                const Statement statement(handle,
                  "INSERT INTO short_term_memory (group_id, memory_content) VALUES (?, ?) "
                  "ON CONFLICT(group_id) DO UPDATE SET memory_content = excluded.memory_content, "
                  "updated_at = CURRENT_TIMESTAMP");
                statement.bind(1, sessionId);
                statement.bind(2, *result.shortTerm);
                statement.execOrThrow();
            }

            std::unordered_set<i64> replacedIds;
            for (const PreparedLongTermMemory &memory: result.longTerm) {
                const Statement statement(
                  handle, "INSERT INTO long_term_memory (group_id, content, embedding) VALUES (?, ?, ?)");
                statement.bind(1, sessionId);
                statement.bind(2, memory.content);
                statement.bind(3, toBytes(memory.embedding));
                statement.execOrThrow();
                replacedIds.insert(memory.replacedIds.begin(), memory.replacedIds.end());
            }
            for (const i64 memoryId: replacedIds) {
                const Statement statement(handle, "DELETE FROM long_term_memory WHERE id = ? AND group_id = ?");
                statement.bind(1, memoryId);
                statement.bind(2, sessionId);
                statement.execOrThrow();
            }

            for (const auto &[qqNumber, delta]: affinity) {
                const Statement statement(handle,
                  "INSERT INTO group_affinity (group_id, qq_number, affinity) VALUES (?, ?, max(min(?, 100), -100)) "
                  "ON CONFLICT(group_id, qq_number) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
                statement.bind(1, sessionId);
                statement.bind(2, qqNumber);
                statement.bind(3, delta);
                statement.bind(4, delta);
                statement.execOrThrow();
            }
            execOrThrow(handle, "COMMIT");
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

} // namespace insoulforge::MemoryMaintenanceStore
