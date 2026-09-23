/// @file MessageList.hpp
/// @brief 单会话完整消息列表

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <deque>
#include <mutex>
#include <optional>

#include <infrastructure/JsonUtil.hpp>

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
    /// @details 列表是该会话 Router、Executor 与工具的唯一运行时消息读源。数据库只用于启动恢复和
    ///          异常退出兜底；每次更新生成独立 JSON 值快照，不会被后续消息修改。
    class MessageList {
    public:
        /// @brief 从数据库恢复一个统一会话的最近完整消息
        /// @param sessionId 群号或带私聊标志位的用户 QQ 号
        explicit MessageList(u64 sessionId);

        /// @brief 追加完整消息并生成冻结快照
        /// @param message 完整消息 JSON；内部不会保留工作流专用 `session_id`
        /// @return 成功插入时返回受限快照与可能产生的记忆总结批次；相同非空消息 ID 已存在时返回空值
        [[nodiscard]] std::optional<MessageListAppendResult> append(json message);

        /// @brief 应用已经成功完成的记忆总结，并删除其对应的最旧消息
        /// @return 删除后若再次达到阈值，返回下一批待持久化的总结任务
        /// @note 线程安全。只删除已由记忆任务成功提交的批次，不会删除总结中的消息。
        [[nodiscard]] std::optional<MemorySummaryBatch> removeCompletedSummaryMessages();

        /// @brief 取消未能持久化的总结批次保留状态
        /// @note 线程安全。仅在任务尚未写入数据库时调用。
        void cancelSummaryBatch();

        /// @brief 获取模型可见的近期消息值快照
        [[nodiscard]] json snapshot() const;

        /// @brief 获取管理后台与持久化恢复使用的完整消息值快照
        /// @note 线程安全。与模型上下文窗口无关，不会截断尚未总结删除的较早消息。
        [[nodiscard]] json fullSnapshot() const;

        /// @brief 将当前完整列表持久化为会话的恢复副本
        void flushToStorage() const;

    private:
        /// @brief 生成锁保护下模型可见的最近消息快照
        [[nodiscard]] json snapshotLocked() const;

        /// @brief 生成锁保护下用于恢复的完整消息快照
        [[nodiscard]] json fullSnapshotLocked() const;

        /// @brief 在未存在总结任务且达到阈值时保留一批最旧消息供异步总结
        [[nodiscard]] std::optional<MemorySummaryBatch> createSummaryBatchLocked();

        u64 m_sessionId; ///< 绑定的统一会话 ID
        mutable std::mutex m_mutex; ///< 保护消息、总结批次状态与快照生成
        std::deque<json> m_messages; ///< 按时间顺序保存的完整消息
        bool m_summaryBatchPending{false}; ///< 是否已有尚未完成的总结任务
    };
} // namespace insoulforge
