/// @file AffinityMaintenanceStore.cpp
/// @brief 好感度维护任务的持久化存储实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::AffinityMaintenanceStore {
    namespace {
        void executeOrThrow(sqlite3 *database, const char *sql) {
            char *error = nullptr;
            if (sqlite3_exec(database, sql, nullptr, nullptr, &error) == SQLITE_OK) {
                return;
            }
            const std::string message = error ? error : sqlite3_errmsg(database);
            sqlite3_free(error);
            throw DbError(message);
        }
    } // namespace

    std::vector<u64> pendingSessionIds() {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(
          database.handle(), "SELECT DISTINCT session_id FROM affinity_maintenance_jobs ORDER BY session_id");
        std::vector<u64> sessionIds;
        while (statement.step()) {
            sessionIds.push_back(static_cast<u64>(statement.getInt64(0)));
        }
        return sessionIds;
    }

    std::optional<AffinityMaintenanceJob> next(const u64 sessionId) {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "SELECT id, messages, attempt_count FROM affinity_maintenance_jobs WHERE session_id = ? ORDER BY id LIMIT 1");
        statement.bind(1, sessionId);
        if (!statement.step()) {
            return std::nullopt;
        }
        json messages;
        if (!tryParseJson(statement.getText(1), messages) || !messages.is_array()) {
            messages = json::array();
        }
        return AffinityMaintenanceJob{.id = statement.getInt64(0),
          .sessionId = sessionId,
          .messages = std::move(messages),
          .attemptCount = statement.getInt(2)};
    }

    void incrementAttempt(const i64 jobId) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(),
          "UPDATE affinity_maintenance_jobs SET attempt_count = attempt_count + 1, updated_at = CURRENT_TIMESTAMP "
          "WHERE id = ?");
        statement.bind(1, jobId);
        statement.execOrThrow();
    }

    void complete(const i64 jobId, const u64 sessionId, const std::vector<std::pair<u64, i32>> &deltas) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        executeOrThrow(handle, "BEGIN IMMEDIATE");
        try {
            for (const auto &[qqNumber, delta]: deltas) {
                const Statement update(handle,
                  "INSERT INTO group_affinity (group_id, qq_number, affinity) VALUES (?, ?, max(min(?, 100), -100)) "
                  "ON CONFLICT(group_id, qq_number) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
                update.bind(1, sessionId);
                update.bind(2, qqNumber);
                update.bind(3, delta);
                update.bind(4, delta);
                update.execOrThrow();
            }
            const Statement remove(handle, "DELETE FROM affinity_maintenance_jobs WHERE id = ?");
            remove.bind(1, jobId);
            remove.execOrThrow();
            executeOrThrow(handle, "COMMIT");
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
} // namespace insoulforge::AffinityMaintenanceStore
