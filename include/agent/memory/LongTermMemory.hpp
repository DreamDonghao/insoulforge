/// @file LongTermMemory.hpp
/// @brief 长期记忆服务
/// @details 将查询文本向量化，再从 SQLite 中检索相似记忆；写入由记忆维护任务负责。

#pragma once

#include <drogon/utils/coroutine.h>
#include <infrastructure/NumericTypes.hpp>
#include <optional>
#include <string>

/// @brief 按语义检索当前会话的长期记忆
namespace insoulforge::LongTermMemory {
    /// @brief 检索长期记忆（余弦相似度 topK）
    /// @param query 查询文本
    /// @param topK 返回结果数量
    /// @param sessionId 会话 ID
    /// @return 检索结果文本；向量化失败返回 std::nullopt，无相似记忆返回 "未找到相关信息"
    auto searchMemory(std::string query, i32 topK, u64 sessionId) -> drogon::Task<std::optional<std::string>>;
} // namespace insoulforge::LongTermMemory
