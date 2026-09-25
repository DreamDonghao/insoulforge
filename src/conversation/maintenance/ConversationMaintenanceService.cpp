/// @file ConversationMaintenanceService.cpp
/// @brief 会话派生状态维护的统一入口实现


#include <infrastructure/NumericTypes.hpp>

#include <conversation/maintenance/ConversationMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceService.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceService.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>

namespace insoulforge::ConversationMaintenanceService {
    void enqueue(const u64 sessionId, const json &messages, const json &contextMessages) {
        ConversationMaintenanceStore::enqueue(sessionId, messages, contextMessages);
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
    }

    void setMemorySummaryCompletedCallback(std::function<void(u64)> callback) {
        MemoryMaintenanceService::setSummaryCompletedCallback(std::move(callback));
    }

    auto hasPendingMemorySummary(const u64 sessionId) -> bool {
        return MemoryMaintenanceStore::hasUnfinished(sessionId);
    }

    auto takeCompletedMemorySummary(const u64 sessionId) -> std::optional<size_t> {
        return MemoryMaintenanceStore::takeCompleted(sessionId);
    }

    void resumePending() {
        for (const u64 sessionId: MemoryMaintenanceStore::pendingSessionIds()) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        }
        for (const u64 sessionId: AffinityMaintenanceStore::pendingSessionIds()) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
        }
    }
} // namespace insoulforge::ConversationMaintenanceService
