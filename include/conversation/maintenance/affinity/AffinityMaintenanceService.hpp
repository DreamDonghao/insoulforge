/// @file AffinityMaintenanceService.hpp
/// @brief 基于持久化消息批次的好感度维护

#pragma once

#include <drogon/utils/coroutine.h>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::AffinityMaintenanceService {
    /// @brief 处理指定会话已持久化的好感度维护任务
    /// @details 重复调用安全；同一会话已有消费者时立即返回，不会轮询或空转等待。
    drogon::Task<> processPending(u64 sessionId);
} // namespace insoulforge::AffinityMaintenanceService
