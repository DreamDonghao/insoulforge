/// @file SessionConfigManager.cpp
/// @brief 会话消息统计与显示名称管理的实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/session/SessionConfigManager.hpp>

namespace insoulforge {
    auto SessionConfigManager::getConfig(const SessionId sessionId) -> SessionConfig {
        return SessionStore::getSessionConfig(sessionId);
    }

    auto SessionConfigManager::contains(const SessionId sessionId) -> bool {
        return SessionStore::hasSessionConfig(sessionId);
    }

    void SessionConfigManager::addConfig(const SessionId sessionId, const SessionConfig &config) {
        SessionStore::saveSessionConfig(sessionId, config);
    }

    void SessionConfigManager::incrementMessageCount(const SessionId sessionId) {
        SessionStore::incrementMessageCount(sessionId);
    }
    auto SessionConfigManager::fetchAndUpdateSessionName(const SessionId sessionId) -> drogon::Task<std::string> {
        std::string name;
        if (sessionId.isPrivate()) {
            // 私聊会话取 QQ 昵称，复用 groupName 列存储
            const UserId userId{sessionId.targetId()};
            const auto resp = co_await OneBotClient::getStrangerInfo(userId, sessionId);
            name = getStr(atOrNull(resp, "data"), "nickname");
        } else {
            const auto result = co_await OneBotClient::getGroupInfo(sessionId);
            name = getStr(atOrNull(result, "data"), "group_name");
        }
        if (!name.empty()) {
            SessionStore::updateSessionName(sessionId, name);
        }

        co_return name;
    }
} // namespace insoulforge
