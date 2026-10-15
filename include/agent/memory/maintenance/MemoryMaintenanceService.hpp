/// @file MemoryMaintenanceService.hpp
/// @brief 记忆维护协调服务
/// @details 消费 `memory_maintenance_jobs` 中持久化的总结批次，提取并整理短期、长期记忆。
///          每个会话串行处理；结果与任务完成状态在同一事务中提交，进程重启后可继续未完成任务。

#pragma once

#include <functional>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 已记录对话的短期与长期记忆维护协调逻辑
/// @details 本服务负责任务调度、退避重试边界及记忆提取、长期记忆召回与合并。好感度评估使用
///          同一批消息独立异步执行，不阻塞记忆任务消费者。
namespace insoulforge::MemoryMaintenanceService {
    /// @brief 处理指定会话已持久化的记忆维护任务
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @details 重复调用安全；同一会话已有消费者时立即返回，不会轮询或空转等待。
    auto processPending(u64 sessionId) -> drogon::Task<>;

    /// @brief 注册记忆总结成功后的通知回调
    /// @details 回调在记忆结果提交事务完成后调用，用于让消息列表删除已总结的前缀。回调不得抛出异常。
    void setSummaryCompletedCallback(std::function<void(u64)> callback);

} // namespace insoulforge::MemoryMaintenanceService
