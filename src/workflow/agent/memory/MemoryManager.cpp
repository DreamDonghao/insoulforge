/// @file MemoryManager.cpp
/// @brief 会话外号存储读取适配器 - 实现

#include <infrastructure/NumericTypes.hpp>
#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>

namespace insoulforge {
    MemoryManager::MemoryManager(const u64 sessionId) : m_sessionId(sessionId) {}

    auto MemoryManager::getMemory() const -> std::string { return MemoryStore::getShortTermMemory(m_sessionId); }
} // namespace insoulforge
