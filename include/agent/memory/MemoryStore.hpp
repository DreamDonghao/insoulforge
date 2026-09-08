/// @file MemoryStore.hpp
/// @brief 短期记忆存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：short_term_memory（当前会话的短期记忆）

#pragma once
#include <cstdint>
#include <string>


/// @brief 短期记忆存储
namespace insoulforge::MemoryStore {
    [[nodiscard]] std::string getShortTermMemory(uint64_t sessionId);

    void updateShortTermMemory(uint64_t sessionId, const std::string &memory);

} // namespace insoulforge::MemoryStore
