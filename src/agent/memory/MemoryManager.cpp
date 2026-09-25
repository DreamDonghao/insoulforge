/// @file MemoryManager.cpp
/// @brief 短期记忆管理器 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <agent/memory/MemoryManager.hpp>
#include <agent/memory/MemoryStore.hpp>

namespace insoulforge {
    MemoryManager::MemoryManager(u64 sessionId) : m_sessionId(sessionId) {}

    auto MemoryManager::getMemory() const -> std::string { return MemoryStore::getShortTermMemory(m_sessionId); }
} // namespace insoulforge
