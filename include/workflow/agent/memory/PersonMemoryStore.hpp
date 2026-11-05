/// @file PersonMemoryStore.hpp
/// @brief 全局人物档案的上下文读取，不受会话共享开关影响。

#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge::PersonMemoryStore {
    class ConsolidationConflict : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
    /// @brief 按最近聊天的真实发送者去重、批量读取档案，排除机器人、系统和没有记忆的人。
    [[nodiscard]] auto getForSenders(const json &recentConversation) -> json;
    [[nodiscard]] auto listPeople() -> json;
    [[nodiscard]] auto getPerson(const UserId &userId) -> json;
    auto updatePerson(const UserId &userId, const json &expected, const json &replacement) -> bool;
    auto deletePerson(const UserId &userId, const json &expected) -> bool;

    /// @brief 事务内读取实际上下文投影；调用方须持有数据库锁。
    [[nodiscard]] auto contextSnapshot(const UserId &userId) -> json;
    /// @brief 同一写入事务内，投影变化时刷新统计；调用方须持有数据库锁。
    void updateTokenEstimate(const UserId &userId, const json &before);

    struct PendingSnapshot {
        UserId userId;
        i64 lastEntryId = 0;
        json profile;
    };
    struct DailyState {
        std::string completedDay;
        std::string runningDay;
    };
    [[nodiscard]] auto pendingPeople() -> std::vector<UserId>;
    /// @brief 手动整理可包含没有待整理条目的现有档案；空档案不请求模型。
    [[nodiscard]] auto pendingSnapshot(const UserId &userId, bool includeExisting = false)
      -> std::optional<PendingSnapshot>;
    /// @brief 替换压缩快照的三个分类；并发修改时抛冲突，Token 未减少则回滚并返回 false。
    /// @note 不消费待整理条目；统计失败也不保存。
    [[nodiscard]] auto applyCompression(const PendingSnapshot &snapshot, const json &profile) -> bool;
    void applyConsolidation(const PendingSnapshot &snapshot, const json &profile);
    [[nodiscard]] auto dailyState() -> DailyState;
    void saveDailyState(const DailyState &state);
} // namespace insoulforge::PersonMemoryStore
