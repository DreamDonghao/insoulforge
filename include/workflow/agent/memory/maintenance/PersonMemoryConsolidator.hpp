/// @file PersonMemoryConsolidator.hpp
/// @brief 全局人物记忆每日整理与启动补跑。
#pragma once

#include <mutex>
#include <optional>

#include <ctime>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>

namespace insoulforge {
    class PersonMemoryConsolidator {
    public:
        static auto instance() -> PersonMemoryConsolidator &;
        void start(trantor::EventLoop &loop);
        void stop();
        /// @brief 按本地日历计算应执行的日期；首次启用在 03:00 前不补昨天。
        [[nodiscard]] static auto dueDay(std::time_t now, const PersonMemoryStore::DailyState &state)
          -> std::optional<std::string>;
        /// @brief 执行到期的一轮；同一进程不允许重叠，停止后不再访问数据库。
        auto runDue(std::time_t now) -> drogon::Task<>;

    private:
        void poll();
        auto consolidatePerson(u64 qq, std::shared_ptr<LlmClient::MaintenanceRequestControl> control)
          -> drogon::Task<bool>;
        std::mutex m_mutex;
        bool m_busy = false;
        std::shared_ptr<LlmClient::MaintenanceRequestControl> m_requestControl =
          std::make_shared<LlmClient::MaintenanceRequestControl>();
        trantor::EventLoop *m_loop = nullptr;
        trantor::TimerId m_timer{};
    };
} // namespace insoulforge
