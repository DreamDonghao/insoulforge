/// @file LongTermMemory.hpp
/// @brief 长期记忆服务
/// @author donghao
/// @date 2026-09-01
/// @details 本地长期记忆存取（SQLite 向量检索，embedding 走 OpenAI 兼容 API）：
///          - 记忆检索：searchMemory()

#pragma once

#include <drogon/utils/coroutine.h>
#include <infrastructure/NumericTypes.hpp>
#include <optional>
#include <string>

/// @brief 长期记忆服务 - 封装记忆的向量化写入与相似度检索
namespace insoulforge::LongTermMemory {
    /// @brief 检索长期记忆（余弦相似度 topK）
    /// @param query 查询文本
    /// @param topK 返回结果数量
    /// @param sessionId 会话 ID
    /// @return 检索结果文本；向量化失败返回 std::nullopt，无相似记忆返回 "未找到相关信息"
    auto searchMemory(std::string query, i32 topK, u64 sessionId) -> drogon::Task<std::optional<std::string>>;
} // namespace insoulforge::LongTermMemory
