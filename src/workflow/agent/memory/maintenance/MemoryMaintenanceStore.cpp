/// @file MemoryMaintenanceStore.cpp
/// @brief 聊天记忆与好感度结果的事务存储实现

#include <unordered_set>

#include <cstring>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
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

    void apply(const Database &database, const u64 sessionId, const MemoryMaintenanceResult &result) {
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

            for (const PreparedLongTermMemory &memory: result.longTerm) {
                const Statement statement(
                  handle, "INSERT INTO long_term_memory (group_id, content, embedding) VALUES (?, ?, ?)");
                statement.bind(1, sessionId);
                statement.bind(2, memory.content);
                statement.bind(3, toBytes(memory.embedding));
                statement.execOrThrow();
            }
            const std::unordered_set<i64> replacedIds(result.replacedLongTermIds.begin(), result.replacedLongTermIds.end());
            for (const i64 memoryId: replacedIds) {
                const Statement statement(handle, "DELETE FROM long_term_memory WHERE id = ? AND group_id = ?");
                statement.bind(1, memoryId);
                statement.bind(2, sessionId);
                statement.execOrThrow();
            }

            for (const auto &[qqNumber, content]: result.personEntries) {
                const Statement profile(handle, "INSERT OR IGNORE INTO person_memories (qq_number) VALUES (?)");
                profile.bind(1, qqNumber);
                profile.execOrThrow();
                const Statement entry(
                  handle, "INSERT INTO person_memory_entries (qq_number, source_session, content) VALUES (?, ?, ?)");
                entry.bind(1, qqNumber);
                entry.bind(2, sessionId);
                entry.bind(3, content);
                entry.execOrThrow();
            }

            for (const auto &[qqNumber, delta]: result.affinity) {
                const Statement statement(handle,
                  "INSERT INTO person_affinity (qq_number, affinity) VALUES (?, max(min(?, 100), -100)) "
                  "ON CONFLICT(qq_number) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
                statement.bind(1, qqNumber);
                statement.bind(2, delta);
                statement.bind(3, delta);
                statement.execOrThrow();
            }
            execOrThrow(handle, "COMMIT");
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

} // namespace insoulforge::MemoryMaintenanceStore
