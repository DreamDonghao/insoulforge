/// @file MemoryMaintenanceQueueStore.hpp
/// @brief 记忆维护任务存储，保证聊天记忆总结和好感度评估任务同时写入

#pragma once


#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class Database;
}

namespace insoulforge::MemoryMaintenanceQueueStore {
    /// @brief 使用指定数据库，在同一事务中创建一批记忆维护任务。
    void enqueue(const Database &database, u64 sessionId, const json &messages, const json &contextMessages);

    /// @brief 为同一消息批次创建聊天记忆总结和好感度评估任务
    /// @param sessionId 会话 ID
    /// @param messages 真正参与记忆提取与好感度评估的完整消息
    /// @param contextMessages 仅帮助记忆提取理解语境的后续消息
    /// @details 记忆与好感度任务处于同一 SQLite 事务，任一任务未写入时整个批次不会提交。
    void enqueue(u64 sessionId, const json &messages, const json &contextMessages);
} // namespace insoulforge::MemoryMaintenanceQueueStore
