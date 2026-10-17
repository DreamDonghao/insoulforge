/// @file SessionConfigManager.cpp
/// @brief 会话消息统计与显示名称管理的实现

#include <infrastructure/NumericTypes.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/SessionConfigManager.hpp>

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
    auto SessionConfigManager::fetchAndUpdateSessionName(const u64 sessionId) -> drogon::Task<std::string> {
        std::string name;
        if (SessionId::isPrivate(sessionId)) {
            // 私聊会话取 QQ 昵称，复用 groupName 列存储
            const u64 userId = SessionId::privateUserId(sessionId);
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
