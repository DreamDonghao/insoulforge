/// @file ConversationMaintenanceService.cpp
/// @brief 会话派生状态维护的统一入口实现


#include <conversation/maintenance/ConversationMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceService.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceService.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>

namespace insoulforge::ConversationMaintenanceService {
    void enqueue(const uint64_t sessionId, const json &messages, const json &contextMessages) {
        ConversationMaintenanceStore::enqueue(sessionId, messages, contextMessages);
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        drogon::async_run(
          [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
    }

    void setMemorySummaryCompletedCallback(std::function<void(uint64_t)> callback) {
        MemoryMaintenanceService::setSummaryCompletedCallback(std::move(callback));
    }

    bool hasPendingMemorySummary(const uint64_t sessionId) { return MemoryMaintenanceStore::hasUnfinished(sessionId); }

    std::optional<size_t> takeCompletedMemorySummary(const uint64_t sessionId) {
        return MemoryMaintenanceStore::takeCompleted(sessionId);
    }

    void resumePending() {
        for (const uint64_t sessionId: MemoryMaintenanceStore::pendingSessionIds()) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await MemoryMaintenanceService::processPending(sessionId); });
        }
        for (const uint64_t sessionId: AffinityMaintenanceStore::pendingSessionIds()) {
            drogon::async_run(
              [sessionId]() -> drogon::Task<> { co_await AffinityMaintenanceService::processPending(sessionId); });
        }
    }
} // namespace insoulforge::ConversationMaintenanceService
