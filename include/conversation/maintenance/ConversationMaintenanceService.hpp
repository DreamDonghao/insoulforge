/// @file ConversationMaintenanceService.hpp
/// @brief 会话派生状态维护的统一入口

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <cstdint>
#include <functional>
#include <optional>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::ConversationMaintenanceService {
    /// @brief 原子持久化一批会话派生状态维护任务并启动消费者
    /// @param sessionId 会话 ID
    /// @param messages 参与记忆提取与好感度评估的完整消息
    /// @param contextMessages 仅供记忆提取理解语境的后续消息
    void enqueue(u64 sessionId, const json &messages, const json &contextMessages);

    /// @brief 注册记忆任务完成后的通知回调
    /// @details 回调用于让消息列表删除已完成总结的前缀；回调不得抛出异常。
    void setMemorySummaryCompletedCallback(std::function<void(u64)> callback);

    /// @brief 判断会话是否存在尚未完成或尚未应用删除的记忆总结任务
    [[nodiscard]] bool hasPendingMemorySummary(u64 sessionId);

    /// @brief 确认已完成总结的消息前缀删除，并返回其数量
    /// @return 没有待确认的已完成任务时返回空值
    [[nodiscard]] std::optional<size_t> takeCompletedMemorySummary(u64 sessionId);

    /// @brief 恢复并调度所有遗留的会话派生状态维护任务
    void resumePending();
} // namespace insoulforge::ConversationMaintenanceService
