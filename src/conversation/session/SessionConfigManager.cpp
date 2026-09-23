/// @file SessionConfigManager.cpp
/// @brief 群组配置管理器 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/session/SessionConfigManager.hpp>

namespace insoulforge {
    SessionConfig SessionConfigManager::getConfig(const u64 sessionId) {
        return SessionStore::getSessionConfig(sessionId);
    }

    bool SessionConfigManager::contains(const u64 sessionId) { return SessionStore::hasSessionConfig(sessionId); }

    void SessionConfigManager::addConfig(const u64 sessionId, const SessionConfig &config) {
        SessionStore::saveSessionConfig(sessionId, config);
    }

    void SessionConfigManager::incrementMessageCount(const u64 sessionId) {
        SessionStore::incrementMessageCount(sessionId);
    }
} // namespace insoulforge
