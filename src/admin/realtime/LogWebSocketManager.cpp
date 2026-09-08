/// @file LogWebSocketManager.cpp
/// @brief 运行日志 WebSocket 管理器 - 实现

#include <admin/realtime/LogWebSocketManager.hpp>
#include <utility>

namespace insoulforge {
    LogWebSocketManager &LogWebSocketManager::instance() {
        static LogWebSocketManager manager;
        return manager;
    }

    void LogWebSocketManager::addConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.insert(conn);
    }

    void LogWebSocketManager::removeConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.erase(conn);
        m_subscriptions.erase(conn);
    }

    void LogWebSocketManager::updateSubscription(
      const drogon::WebSocketConnectionPtr &conn, LogSubscription subscription) {
        std::lock_guard lock(m_mutex);
        m_subscriptions[conn] = std::move(subscription);
    }

    void LogWebSocketManager::pushLog(const json &log) {
        std::lock_guard lock(m_mutex);
        for (const auto &conn: m_connections) {
            auto it = m_subscriptions.find(conn);
            if (it != m_subscriptions.end() && matches(it->second, log)) {
                json msg;
                msg["type"] = "log";
                msg["data"] = log;
                conn->send(dumpJson(msg));
            }
        }
    }

    void LogWebSocketManager::broadcastStatus(const json &status) {
        std::lock_guard lock(m_mutex);
        json msg;
        msg["type"] = "status";
        msg["data"] = status;
        const auto jsonStr = dumpJson(msg);
        for (const auto &conn: m_connections) {
            conn->send(jsonStr);
        }
    }

    bool LogWebSocketManager::matches(const LogSubscription &subscription, const json &log) {
        // 日志条目的会话字段线上名为 "groupId"（字符串，可能带私聊标志位超出 int64）
        const json &groupIdVal = atOrNull(log, "groupId");
        const bool hasSession = !groupIdVal.is_null();
        if (subscription.systemOnly) {
            if (hasSession) {
                return false;
            }
        } else if (subscription.sessionId.has_value()) {
            if (!hasSession || parseUInt64(jsonToString(groupIdVal)) != *subscription.sessionId) {
                return false;
            }
        }
        if (subscription.level.has_value() && getStr(log, "level") != *subscription.level) {
            return false;
        }
        if (!subscription.keyword.empty() && getStr(log, "message").find(subscription.keyword) == std::string::npos) {
            return false;
        }
        return true;
    }
} // namespace insoulforge
