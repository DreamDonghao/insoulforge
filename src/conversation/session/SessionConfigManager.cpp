/// @file SessionConfigManager.cpp
/// @brief 会话消息统计接口的实现

#include <conversation/session/SessionConfigManager.hpp>
#include <infrastructure/NumericTypes.hpp>

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
