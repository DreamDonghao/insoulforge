/// @file ConversationMaintenanceStore.hpp
/// @brief 会话派生状态维护任务的原子持久化边界

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <cstdint>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::ConversationMaintenanceStore {
    /// @brief 原子创建同一消息批次的全部派生状态维护任务
    /// @param sessionId 会话 ID
    /// @param messages 真正参与记忆提取与好感度评估的完整消息
    /// @param contextMessages 仅帮助记忆提取理解语境的后续消息
    /// @details 记忆与好感度任务处于同一 SQLite 事务，任一任务未写入时整个批次不会提交。
    void enqueue(u64 sessionId, const json &messages, const json &contextMessages);
} // namespace insoulforge::ConversationMaintenanceStore
