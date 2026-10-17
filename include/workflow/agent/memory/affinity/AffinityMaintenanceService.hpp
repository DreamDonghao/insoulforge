/// @file AffinityMaintenanceService.hpp
/// @brief 从消息批次评估好感度变化，不写入数据库

#pragma once

#include <optional>

#include <drogon/utils/coroutine.h>

#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::AffinityMaintenanceService {
    /// @brief 计算好感度变化；失败返回空值，空列表表示无需调整
    auto prepare(u64 sessionId, json messages) -> drogon::Task<std::optional<AffinityDeltas>>;
} // namespace insoulforge::AffinityMaintenanceService
