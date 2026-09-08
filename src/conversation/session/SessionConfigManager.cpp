/// @file SessionConfigManager.cpp
/// @brief 群组配置管理器 - 实现

#include <conversation/session/SessionConfigManager.hpp>
#include <conversation/session/SessionStore.hpp>

namespace insoulforge {
    SessionConfig SessionConfigManager::getConfig(const uint64_t sessionId) {
        return SessionStore::getSessionConfig(sessionId);
    }

    bool SessionConfigManager::contains(const uint64_t sessionId) { return SessionStore::hasSessionConfig(sessionId); }

    void SessionConfigManager::addConfig(const uint64_t sessionId, const SessionConfig &config) {
        SessionStore::saveSessionConfig(sessionId, config);
    }

    void SessionConfigManager::incrementMessageCount(const uint64_t sessionId) {
        SessionStore::incrementMessageCount(sessionId);
    }
} // namespace insoulforge
