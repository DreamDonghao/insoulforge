/// @file MemoryMaintenanceCoordinator.cpp
/// @brief 记忆维护入口实现，调度聊天记忆总结和好感度评估


#include <agent/memory/maintenance/MemoryMaintenanceQueueStore.hpp>
#include <agent/memory/affinity/AffinityMaintenanceService.hpp>
#include <agent/memory/affinity/AffinityMaintenanceStore.hpp>
#include <agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>

namespace insoulforge::MemoryMaintenanceCoordinator {
    void enqueue(const Database &database, const u64 sessionId, const json &messages, const json &contextMessages) {
        MemoryMaintenanceQueueStore::enqueue(database, sessionId, messages, contextMessages);
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
    }

    void setMemorySummaryCompletedCallback(std::function<void(u64)> callback) {
        MemoryMaintenanceService::setSummaryCompletedCallback(std::move(callback));
    }

    auto hasPendingMemorySummary(const Database &database, const u64 sessionId) -> bool {
        return MemoryMaintenanceStore::hasUnfinished(database, sessionId);
    }

    auto takeCompletedMemorySummary(const Database &database, const u64 sessionId) -> std::optional<size_t> {
        return MemoryMaintenanceStore::takeCompleted(database, sessionId);
    }

    void resumePending(const Database &database) {
        // 先读取两类任务，避免第二次查询失败时已经启动了一部分消费者。
        const auto memorySessions = MemoryMaintenanceStore::pendingSessionIds(database);
        const auto affinitySessions = AffinityMaintenanceStore::pendingSessionIds(database);
        for (const u64 sessionId: memorySessions) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        }
        for (const u64 sessionId: affinitySessions) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
        }
    }

    void enqueue(const u64 sessionId, const json &messages, const json &contextMessages) {
        enqueue(Database::instance(), sessionId, messages, contextMessages);
    }
    auto hasPendingMemorySummary(const u64 sessionId) -> bool {
        return hasPendingMemorySummary(Database::instance(), sessionId);
    }
    auto takeCompletedMemorySummary(const u64 sessionId) -> std::optional<size_t> {
        return takeCompletedMemorySummary(Database::instance(), sessionId);
    }
} // namespace insoulforge::MemoryMaintenanceCoordinator
