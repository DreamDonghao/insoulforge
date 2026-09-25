/// @file SessionConfigManager.cpp
/// @brief 群组配置管理器 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/session/SessionConfigManager.hpp>

namespace insoulforge {
    auto SessionConfigManager::getConfig(const u64 sessionId) -> SessionConfig {
        return SessionStore::getSessionConfig(sessionId);
    }

    auto SessionConfigManager::contains(const u64 sessionId) -> bool {
        return SessionStore::hasSessionConfig(sessionId);
    }

    void SessionConfigManager::addConfig(const u64 sessionId, const SessionConfig &config) {
        SessionStore::saveSessionConfig(sessionId, config);
    }

    void SessionConfigManager::incrementMessageCount(const u64 sessionId) {
        SessionStore::incrementMessageCount(sessionId);
    }
} // namespace insoulforge
