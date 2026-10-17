/// @file MessageList.hpp
/// @brief 单会话完整消息列表

#pragma once

#include <deque>
#include <mutex>
#include <optional>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class Database;
    class Config;
    /// @brief 一批待提取记忆的消息及其只读上下文
    struct MemorySummaryBatch {
        json messages; ///< 真正参与记忆提取、成功后从列表删除的最旧消息
        json contextMessages; ///< 仅帮助模型理解语境的后续消息，不参与提取或删除
    };

    /// @brief 管理一个会话的完整消息列表
    /// @details 列表是该会话 Router、Executor 与工具的运行时消息来源。启动时从数据库恢复，
    ///          正常退出时写回数据库；每次更新返回独立快照，不受后续消息影响。
    class MessageList {
    public:
        /// @brief 从数据库恢复一个统一会话的最近完整消息
        /// @param sessionId 群号或带私聊标志位的用户 QQ 号
        /// @param database 已成功初始化的数据库，供恢复和持久化使用。
        /// @param config 消息窗口与总结参数；两个依赖必须覆盖消息列表的生命周期。
        MessageList(u64 sessionId, const Database &database, const Config &config);

        /// @brief 追加完整消息并生成冻结快照
        /// @param message 完整消息 JSON；内部不会保留工作流专用 `session_id`
        /// @return 插入成功时返回模型窗口内的快照；非空消息 ID 重复时返回空值
        [[nodiscard]] auto append(json message) -> std::optional<json>;

        /// @brief 结束维护任务；成功时按数量删除队首消息，失败时传入 0 保留消息
        /// @note 手动清空不会取消任务，旧任务完成时仍可能删除清空后收到的新消息。
        void finishSummaryBatch(size_t removeCount);

        /// @brief 在没有待处理总结任务且消息数达到或超过阈值时，预留一批最旧消息
        /// @note 线程安全。由工作流在消息预处理结束后调用，追加消息本身不触发总结。
        [[nodiscard]] auto createSummaryBatch() -> std::optional<MemorySummaryBatch>;

        /// @brief 获取模型可见的近期消息值快照
        [[nodiscard]] auto snapshot() const -> json;

        /// @brief 获取管理后台与持久化恢复使用的完整消息值快照
        /// @note 线程安全。与模型上下文窗口无关，不会截断尚未总结删除的较早消息。
        [[nodiscard]] auto fullSnapshot() const -> json;

        /// @brief 将当前完整列表持久化为会话的恢复副本
        void flushToStorage() const;

        /// @brief 清空内存消息和数据库恢复副本；不取消正在执行的回复或记忆任务。
        /// @note 维护完成后仍按原批次数量删除队首，可能删到清空后收到的新消息。
        void clear();

    private:
        /// @brief 生成锁保护下模型可见的最近消息快照
        [[nodiscard]] auto snapshotLocked() const -> json;

        /// @brief 生成锁保护下用于恢复的完整消息快照
        [[nodiscard]] auto fullSnapshotLocked() const -> json;

        u64 m_sessionId; ///< 绑定的统一会话 ID
        const Database &m_database;
        const Config &m_config;
        mutable std::mutex m_mutex; ///< 保护消息、总结批次状态与快照生成
        std::deque<json> m_messages; ///< 按时间顺序保存的完整消息
        bool m_summaryBatchPending{false}; ///< 是否有正在执行的内存维护任务
    };
} // namespace insoulforge
