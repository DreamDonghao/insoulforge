/// @file LongTermMemory.hpp
/// @brief 长期记忆服务
/// @details 将查询文本向量化，再从 SQLite 中检索相似记忆；写入由记忆维护任务负责。

#pragma once

#include <expected>
#include <optional>
#include <string>

#include <drogon/utils/coroutine.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>

/// @brief 按语义检索当前会话的长期记忆
namespace insoulforge::LongTermMemory {
    /// @brief 检索长期记忆（余弦相似度 topK）
    /// @param query 查询文本
    /// @param topK 返回结果数量
    /// @param sessionId 会话 ID
    /// @return 无相似记忆时返回成功的空值；向量化失败时返回原因
    auto searchMemory(std::string query, i32 topK, SessionId sessionId)
      -> drogon::Task<std::expected<std::optional<std::string>, std::string>>;
} // namespace insoulforge::LongTermMemory
