/// @file MemoryMaintenanceCoordinator.cpp
/// @brief 记忆维护批次计算与统一提交

#include <workflow/agent/memory/affinity/AffinityMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge::MemoryMaintenanceCoordinator {
    auto process(const Database &database, const u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<bool> {
        const auto memory = co_await MemoryMaintenanceService::prepare(sessionId, messages, std::move(contextMessages));
        if (!memory) {
            co_return false;
        }
        const auto affinity = co_await AffinityMaintenanceService::prepare(sessionId, std::move(messages));
        if (!affinity) {
            co_return false;
        }
        MemoryMaintenanceStore::apply(database, sessionId, *memory, *affinity);
        Logger::info(sessionId, "Memory",
          fmt::format("记忆维护完成: 长期记忆 +{}，好感度更新 {} 人", memory->longTerm.size(), affinity->size()));
        co_return true;
    }
} // namespace insoulforge::MemoryMaintenanceCoordinator
