/// @file MemoryMaintenanceService.hpp
/// @brief 联合提取人物记忆与全局好感度，并沿用现有会话记忆整理，不写入数据库

#pragma once

#include <optional>

#include <drogon/utils/coroutine.h>

#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MemoryMaintenanceService {
    /// @brief 计算人物条目、好感度及短期/长期记忆变更；失败返回空值，空结果仍表示成功
    auto prepare(u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<std::optional<MemoryMaintenanceResult>>;
} // namespace insoulforge::MemoryMaintenanceService
