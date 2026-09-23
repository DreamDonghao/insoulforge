/// @file MemoryMaintenanceStore.cpp
/// @brief 记忆维护任务及其提交结果的持久化存储实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::MemoryMaintenanceStore {
    namespace {
        /// @brief 把 f32 向量转换为 SQLite BLOB
        [[nodiscard]] std::vector<u8> toBytes(const std::vector<f32> &embedding) {
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

    i64 enqueue(const u64 sessionId, const json &messages, const json &contextMessages, const size_t removeCount) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "INSERT INTO memory_maintenance_jobs (session_id, messages, context_messages, remove_count) "
          "VALUES (?, ?, ?, ?)");
        statement.bind(1, sessionId);
        statement.bind(2, dumpJson(messages));
        statement.bind(3, dumpJson(contextMessages));
        statement.bind(4, static_cast<i64>(removeCount));
        statement.execOrThrow();
        return Statement::lastInsertRowId(database.handle());
    }

    std::vector<u64> pendingSessionIds() {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "SELECT DISTINCT session_id FROM memory_maintenance_jobs WHERE status = 'pending' ORDER BY session_id");
        std::vector<u64> sessionIds;
        while (statement.step()) {
            sessionIds.push_back(static_cast<u64>(statement.getInt64(0)));
        }
        return sessionIds;
    }

    bool hasUnfinished(const u64 sessionId) {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(
          database.handle(), "SELECT 1 FROM memory_maintenance_jobs WHERE session_id = ? LIMIT 1");
        statement.bind(1, sessionId);
        return statement.step();
    }

    std::optional<MemoryMaintenanceJob> next(const u64 sessionId) {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "SELECT id, messages, context_messages, remove_count, attempt_count FROM memory_maintenance_jobs "
          "WHERE session_id = ? AND status = 'pending' ORDER BY id LIMIT 1");
        statement.bind(1, sessionId);
        if (!statement.step()) {
            return std::nullopt;
        }

        json messages;
        if (!tryParseJson(statement.getText(1), messages) || !messages.is_array()) {
            messages = json::array();
        }
        json contextMessages;
        if (!tryParseJson(statement.getText(2), contextMessages) || !contextMessages.is_array()) {
            contextMessages = json::array();
        }
        return MemoryMaintenanceJob{.id = statement.getInt64(0),
          .sessionId = sessionId,
          .messages = std::move(messages),
          .contextMessages = std::move(contextMessages),
          .removeCount = static_cast<size_t>(std::max(statement.getInt64(3), i64{0})),
          .attemptCount = statement.getInt(4)};
    }

    std::optional<size_t> takeCompleted(const u64 sessionId) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        const Statement select(handle,
          "SELECT id, remove_count FROM memory_maintenance_jobs WHERE session_id = ? AND status = 'completed' "
          "ORDER BY id LIMIT 1");
        select.bind(1, sessionId);
        if (!select.step()) {
            return std::nullopt;
        }
        const i64 jobId = select.getInt64(0);
        const size_t removeCount = static_cast<size_t>(std::max(select.getInt64(1), i64{0}));
        const Statement remove(handle, "DELETE FROM memory_maintenance_jobs WHERE id = ?");
        remove.bind(1, jobId);
        remove.execOrThrow();
        return removeCount;
    }

    void incrementAttempt(const i64 jobId) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "UPDATE memory_maintenance_jobs SET attempt_count = attempt_count + 1, updated_at = CURRENT_TIMESTAMP "
          "WHERE id = ?");
        statement.bind(1, jobId);
        statement.execOrThrow();
    }

    void complete(const i64 jobId, const u64 sessionId, const std::optional<std::string> &shortTermMemory,
      const std::vector<PreparedLongTermMemory> &longTermMemories) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        execOrThrow(handle, "BEGIN IMMEDIATE");
        try {
            if (shortTermMemory) {
                const Statement statement(handle,
                  "INSERT INTO short_term_memory (group_id, memory_content) VALUES (?, ?) "
                  "ON CONFLICT(group_id) DO UPDATE SET memory_content = excluded.memory_content, "
                  "updated_at = CURRENT_TIMESTAMP");
                statement.bind(1, sessionId);
                statement.bind(2, *shortTermMemory);
                statement.execOrThrow();
            }

            std::unordered_set<i64> replacedIds;
            for (const PreparedLongTermMemory &memory: longTermMemories) {
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

            const Statement completeJob(handle,
              "UPDATE memory_maintenance_jobs SET status = 'completed', updated_at = CURRENT_TIMESTAMP WHERE id = ?");
            completeJob.bind(1, jobId);
            completeJob.execOrThrow();
            execOrThrow(handle, "COMMIT");
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
} // namespace insoulforge::MemoryMaintenanceStore
