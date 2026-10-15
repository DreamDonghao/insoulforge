/// @file MemoryMaintenanceQueueStore.cpp
/// @brief 记忆维护任务存储实现，在同一事务中创建两类任务

#include <agent/memory/maintenance/MemoryMaintenanceQueueStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::MemoryMaintenanceQueueStore {
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

    void enqueue(const Database &database, const u64 sessionId, const json &messages, const json &contextMessages) {
        std::unique_lock lock(database.mutex());
        sqlite3 *handle = database.handle();
        executeOrThrow(handle, "BEGIN IMMEDIATE");
        try {
            const Statement memoryTask(handle,
              "INSERT INTO memory_maintenance_jobs (session_id, messages, context_messages, remove_count) "
              "VALUES (?, ?, ?, ?)");
            memoryTask.bind(1, sessionId);
            memoryTask.bind(2, dumpJson(messages));
            memoryTask.bind(3, dumpJson(contextMessages));
            memoryTask.bind(4, static_cast<i64>(messages.size()));
            memoryTask.execOrThrow();

            const Statement affinityTask(
              handle, "INSERT INTO affinity_maintenance_jobs (session_id, messages) VALUES (?, ?)");
            affinityTask.bind(1, sessionId);
            affinityTask.bind(2, dumpJson(messages));
            affinityTask.execOrThrow();
            executeOrThrow(handle, "COMMIT");
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    void enqueue(const u64 sessionId, const json &messages, const json &contextMessages) {
        enqueue(Database::instance(), sessionId, messages, contextMessages);
    }
} // namespace insoulforge::MemoryMaintenanceQueueStore
