/// @file MemoryMaintenanceService.hpp
/// @brief 提取人物记忆与全局好感度，整理会话外号和长期记忆，不写入数据库

#pragma once

#include <expected>
#include <string>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>

namespace insoulforge::MemoryMaintenanceService {
    /// @brief 计算人物条目、好感度、外号及长期记忆变更；失败时返回原因；没有待处理消息时返回空变更集
    auto prepare(SessionId sessionId, json messages, json contextMessages)
      -> drogon::Task<std::expected<MemoryMaintenanceResult, std::string>>;
} // namespace insoulforge::MemoryMaintenanceService
