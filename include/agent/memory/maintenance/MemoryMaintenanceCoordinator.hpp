/// @file MemoryMaintenanceCoordinator.hpp
/// @brief 计算聊天记忆与好感度，全部成功后统一保存

#pragma once

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class Database;
}

namespace insoulforge::MemoryMaintenanceCoordinator {
    /// @brief 处理一个内存批次；失败不提交结果，由后续消息重新触发
    /// @note 调用方负责同会话互斥、异常处理和消息列表收尾。
    auto process(const Database &database, u64 sessionId, json messages, json contextMessages) -> drogon::Task<bool>;
} // namespace insoulforge::MemoryMaintenanceCoordinator
