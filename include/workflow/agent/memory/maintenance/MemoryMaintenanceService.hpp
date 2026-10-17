/// @file MemoryMaintenanceService.hpp
/// @brief 从消息批次提取并整理聊天记忆，不写入数据库

#pragma once

#include <optional>

#include <drogon/utils/coroutine.h>

#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MemoryMaintenanceService {
    /// @brief 计算短期和长期记忆变更；失败返回空值，空结果仍表示成功
    auto prepare(u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<std::optional<MemoryMaintenanceResult>>;
} // namespace insoulforge::MemoryMaintenanceService
