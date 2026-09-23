/// @file AffinityMaintenanceStore.hpp
/// @brief 好感度维护任务的持久化存储

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge {
    /// @brief 等待好感度评估的消息批次
    struct AffinityMaintenanceJob {
        i64 id{0}; ///< 任务主键
        u64 sessionId{0}; ///< 所属会话 ID
        json messages; ///< 按时间顺序排列的完整消息
        i32 attemptCount{0}; ///< 已失败尝试次数
    };

    namespace AffinityMaintenanceStore {
        /// @brief 获取拥有待处理好感度任务的会话 ID
        [[nodiscard]] std::vector<u64> pendingSessionIds();

        /// @brief 获取会话最早的待处理好感度任务
        [[nodiscard]] std::optional<AffinityMaintenanceJob> next(u64 sessionId);

        /// @brief 记录一次失败尝试
        void incrementAttempt(i64 jobId);

        /// @brief 原子应用好感度变化并删除已完成任务
        /// @details 所有分值更新与任务删除处于同一 SQLite 事务，任务重试不会重复叠加分数。
        void complete(i64 jobId, u64 sessionId, const std::vector<std::pair<u64, i32>> &deltas);
    } // namespace AffinityMaintenanceStore
} // namespace insoulforge
