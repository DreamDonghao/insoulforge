/// @file MessageList.hpp
/// @brief 单会话完整消息列表

#pragma once

#include <deque>
#include <mutex>
#include <optional>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 一批待提取记忆的消息及其只读上下文
    struct MemorySummaryBatch {
        json messages; ///< 真正参与记忆提取、成功后从列表删除的最旧消息
        json contextMessages; ///< 仅帮助模型理解语境的后续消息，不参与提取或删除
    };

    /// @brief 一次成功消息列表更新的冻结结果
    struct MessageListAppendResult {
        json messageSnapshot; ///< 更新完成后按时间顺序排列的完整消息快照
        std::optional<MemorySummaryBatch> summaryBatch; ///< 本次触发的记忆总结批次
    };

    /// @brief 管理一个会话的完整消息列表
    /// @details 列表是该会话 Router、Executor 与工具的运行时消息来源。启动时从数据库恢复，
    ///          正常退出时写回数据库；每次更新返回独立快照，不受后续消息影响。
    class MessageList {
    public:
        /// @brief 从数据库恢复一个统一会话的最近完整消息
        /// @param sessionId 群号或带私聊标志位的用户 QQ 号
        explicit MessageList(u64 sessionId);

        /// @brief 追加完整消息并生成冻结快照
        /// @param message 完整消息 JSON；内部不会保留工作流专用 `session_id`
        /// @return 插入成功时返回模型窗口内的快照和可能触发的总结批次；非空消息 ID 重复时返回空值
        [[nodiscard]] auto append(json message) -> std::optional<MessageListAppendResult>;

        /// @brief 删除已完成总结的消息前缀
        /// @return 删除后若再次达到阈值，返回下一批待持久化的总结任务
        /// @note 线程安全。只删除已由记忆任务成功提交的批次，不会删除总结中的消息。
        [[nodiscard]] auto removeCompletedSummaryMessages() -> std::optional<MemorySummaryBatch>;

        /// @brief 取消未能持久化的总结批次保留状态
        /// @note 线程安全。仅在任务尚未写入数据库时调用。
        void cancelSummaryBatch();

        /// @brief 获取模型可见的近期消息值快照
        [[nodiscard]] auto snapshot() const -> json;

        /// @brief 获取管理后台与持久化恢复使用的完整消息值快照
        /// @note 线程安全。与模型上下文窗口无关，不会截断尚未总结删除的较早消息。
        [[nodiscard]] auto fullSnapshot() const -> json;

        /// @brief 将当前完整列表持久化为会话的恢复副本
        void flushToStorage() const;

    private:
        /// @brief 生成锁保护下模型可见的最近消息快照
        [[nodiscard]] auto snapshotLocked() const -> json;

        /// @brief 生成锁保护下用于恢复的完整消息快照
        [[nodiscard]] auto fullSnapshotLocked() const -> json;

        /// @brief 在未存在总结任务且达到阈值时保留一批最旧消息供异步总结
        [[nodiscard]] auto createSummaryBatchLocked() -> std::optional<MemorySummaryBatch>;

        u64 m_sessionId; ///< 绑定的统一会话 ID
        mutable std::mutex m_mutex; ///< 保护消息、总结批次状态与快照生成
        std::deque<json> m_messages; ///< 按时间顺序保存的完整消息
        bool m_summaryBatchPending{false}; ///< 是否已有尚未完成的总结任务
    };
} // namespace insoulforge
