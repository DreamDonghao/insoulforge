/// @file MemoryManager.cpp
/// @brief 会话外号存储读取适配器 - 实现

#include <utility>

#include <infrastructure/SessionId.hpp>
#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>

namespace insoulforge {
    MemoryManager::MemoryManager(SessionId sessionId) : m_sessionId(std::move(sessionId)) {}

    auto MemoryManager::getMemory() const -> std::string { return MemoryStore::getShortTermMemory(m_sessionId); }
} // namespace insoulforge
