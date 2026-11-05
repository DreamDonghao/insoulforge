/// @file PersonMemoryConsolidator.hpp
/// @brief 全局人物记忆每日整理、启动补跑与手动整理。
#pragma once

#include <mutex>
#include <optional>

#include <ctime>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <infrastructure/UserId.hpp>
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
        enum class Result { Updated, NoMemory, Busy, Stopped, Failed, Conflict, Unchanged };
        /// @brief 立即整理或压缩一个人物，与每日任务互斥，不改变每日执行标记。
        /// @param compress 为 true 时忽略触发阈值，先整理待处理条目，再按配置目标压缩。
        auto runPerson(UserId userId, bool compress = false) -> drogon::Task<Result>;

    private:
        struct BusyGuard {
            PersonMemoryConsolidator &service;
            ~BusyGuard();
        };
        void poll();
        auto consolidatePerson(UserId userId, std::shared_ptr<LlmClient::MaintenanceRequestControl> control,
          bool includeExisting = false) -> drogon::Task<Result>;
        /// @brief 使用独立提示词压缩三个分类，失败保留已整理结果。
        /// @param trigger 自动压缩阈值；nullopt 表示手动操作，不检查阈值。
        auto compressPerson(UserId userId, std::shared_ptr<LlmClient::MaintenanceRequestControl> control,
          std::optional<i32> trigger, i32 target) -> drogon::Task<Result>;
        std::mutex m_mutex;
        bool m_busy = false;
        std::shared_ptr<LlmClient::MaintenanceRequestControl> m_requestControl =
          std::make_shared<LlmClient::MaintenanceRequestControl>();
        trantor::EventLoop *m_loop = nullptr;
        trantor::TimerId m_timer{};
    };
} // namespace insoulforge
