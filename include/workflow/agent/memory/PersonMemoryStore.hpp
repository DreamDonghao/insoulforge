/// @file PersonMemoryStore.hpp
/// @brief 全局人物档案的上下文读取，不受会话共享开关影响。

#pragma once

#include <infrastructure/JsonUtil.hpp>
#include <optional>
#include <vector>

namespace insoulforge::PersonMemoryStore {
    /// @brief 按最近聊天的真实发送者去重、批量读取档案，排除机器人、系统和没有记忆的人。
    [[nodiscard]] auto getForSenders(const json &recentConversation) -> json;
    [[nodiscard]] auto listPeople() -> json;
    [[nodiscard]] auto getPerson(u64 qq) -> json;

    struct PendingSnapshot {
        u64 qq = 0;
        i64 lastEntryId = 0;
        json profile;
    };
    struct DailyState {
        std::string completedDay;
        std::string runningDay;
    };
    [[nodiscard]] auto pendingPeople() -> std::vector<u64>;
    [[nodiscard]] auto pendingSnapshot(u64 qq) -> std::optional<PendingSnapshot>;
    void applyConsolidation(const PendingSnapshot &snapshot, const json &profile);
    [[nodiscard]] auto dailyState() -> DailyState;
    void saveDailyState(const DailyState &state);
} // namespace insoulforge::PersonMemoryStore
