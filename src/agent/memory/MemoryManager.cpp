/// @file MemoryManager.cpp
/// @brief 短期记忆管理器 - 实现

#include <agent/memory/MemoryManager.hpp>
#include <agent/memory/MemoryStore.hpp>

namespace insoulforge {
    MemoryManager::MemoryManager(uint64_t sessionId) : m_sessionId(sessionId) {}

    std::string MemoryManager::getMemory() const { return MemoryStore::getShortTermMemory(m_sessionId); }
} // namespace insoulforge
