/// @file MemoryManager.cpp
/// @brief 短期记忆管理器 - 实现

#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    MemoryManager::MemoryManager(const u64 sessionId) : m_sessionId(sessionId) {}

    auto MemoryManager::getMemory() const -> std::string { return MemoryStore::getShortTermMemory(m_sessionId); }
} // namespace insoulforge
