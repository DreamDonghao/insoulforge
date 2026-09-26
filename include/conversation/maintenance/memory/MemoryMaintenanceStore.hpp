/// @file MemoryMaintenanceStore.hpp
/// @brief 记忆维护任务及其提交结果的持久化存储

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 等待维护的记忆总结批次
    struct MemoryMaintenanceJob {
        i64 id{0};
        u64 sessionId{0};
        json messages; ///< 真正参与提取的消息
        json contextMessages; ///< 仅帮助理解语境的后续消息
        size_t removeCount{0}; ///< 成功后从消息列表移除的最旧消息数
        i32 attemptCount{0};
    };

    /// @brief 已完成向量化、可在事务中写入的长期记忆变更
    struct PreparedLongTermMemory {
        std::string content;
        std::vector<f32> embedding;
        std::vector<i64> replacedIds;
    };

    /// @brief 记忆维护存储
    namespace MemoryMaintenanceStore {
        /// @brief 持久化一批待总结消息及其只读上下文
        /// @return 新任务 ID
        auto enqueue(u64 sessionId, const json &messages, const json &contextMessages, size_t removeCount) -> i64;

        /// @brief 获取仍有待处理任务的会话 ID
        [[nodiscard]] auto pendingSessionIds() -> std::vector<u64>;

        /// @brief 判断会话是否已有尚未完成或尚未应用删除的总结任务
        [[nodiscard]] auto hasUnfinished(u64 sessionId) -> bool;

        /// @brief 获取一个会话最早的待处理任务
        [[nodiscard]] auto next(u64 sessionId) -> std::optional<MemoryMaintenanceJob>;

        /// @brief 取走一个已完成任务的删除数量，并确认移除该任务
        /// @return 不存在已完成任务时返回空值；返回 0 表示历史任务不删除运行时消息
        [[nodiscard]] auto takeCompleted(u64 sessionId) -> std::optional<size_t>;

        /// @brief 记录一次失败处理尝试
        void incrementAttempt(i64 jobId);

        /// @brief 原子提交一次记忆整理结果并将任务标记为已完成
        /// @details 短期记忆更新、长期记忆插入/替换和任务状态更新处于同一 SQLite 事务；调用方必须在
        ///          传入前完成全部 LLM 与 embedding 请求，避免事务跨网络等待。
        /// @param shortTermMemory 有值时覆盖短期记忆；空值时保持原短期记忆
        void complete(i64 jobId, u64 sessionId, const std::optional<std::string> &shortTermMemory,
          const std::vector<PreparedLongTermMemory> &longTermMemories);
    } // namespace MemoryMaintenanceStore
} // namespace insoulforge
