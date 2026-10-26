/// @file MemoryMaintenanceCoordinator.cpp
/// @brief 记忆维护批次计算与统一提交

#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/config/Config.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>

namespace insoulforge::MemoryMaintenanceCoordinator {
    auto process(const Database &database, const u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<bool> {
        const auto memory =
          co_await MemoryMaintenanceService::prepare(sessionId, std::move(messages), std::move(contextMessages));
        if (!memory) {
            co_return false;
        }
        MemoryMaintenanceStore::apply(database, sessionId, *memory);
        auto message = fmt::format("记忆维护完成: 人物条目 +{}，长期记忆 +{}", memory->personEntries.size(), memory->longTerm.size());
        if (Config::instance().affinityEnabled.load(std::memory_order_relaxed))
            message += fmt::format("，好感度更新 {} 人", memory->affinity.size());
        Logger::info(sessionId, "Memory", message);
        co_return true;
    }
} // namespace insoulforge::MemoryMaintenanceCoordinator
