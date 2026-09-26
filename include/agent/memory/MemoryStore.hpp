/// @file MemoryStore.hpp
/// @brief 短期记忆存储
/// @details 表：short_term_memory（当前会话的短期记忆）

#pragma once

#include <string>

#include <infrastructure/NumericTypes.hpp>


/// @brief 短期记忆存储
namespace insoulforge::MemoryStore {
    /// @brief 读取会话短期记忆；不存在时返回空字符串
    [[nodiscard]] auto getShortTermMemory(u64 sessionId) -> std::string;

    /// @brief 保存会话短期记忆；已有记录时覆盖内容并更新时间
    void updateShortTermMemory(u64 sessionId, const std::string &memory);

} // namespace insoulforge::MemoryStore
