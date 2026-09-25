/// @file LogWebSocketManager.cpp
/// @brief 运行日志 WebSocket 管理器 - 实现

#include <admin/realtime/LogWebSocketManager.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <vector>

namespace insoulforge {
    auto LogWebSocketManager::instance() -> LogWebSocketManager & {
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

    void LogWebSocketManager::pushLog(const LogEntry &entry) {
        json data;
        data["id"] = entry.id;
        data["timestamp"] = entry.timestamp;
        data["level"] = entry.level;
        data["sessionId"] = std::to_string(entry.sessionId);
        data["source"] = entry.source;
        data["content"] = entry.content;

        json message;
        message["type"] = "log";
        message["data"] = data;
        const std::string payload = dumpJson(message);

        std::vector<drogon::WebSocketConnectionPtr> recipients;
        {
            std::lock_guard lock(m_mutex);
            for (const auto &conn: m_connections) {
                const auto subscription = m_subscriptions.find(conn);
                if (subscription != m_subscriptions.end() && matches(subscription->second, entry)) {
                    recipients.push_back(conn);
                }
            }
        }
        for (const auto &conn: recipients) {
            conn->send(payload);
        }
    }

    void LogWebSocketManager::broadcastStatus(const json &status) {
        json msg;
        msg["type"] = "status";
        msg["data"] = status;
        const auto jsonStr = dumpJson(msg);

        std::vector<drogon::WebSocketConnectionPtr> connections;
        {
            std::lock_guard lock(m_mutex);
            connections.assign(m_connections.begin(), m_connections.end());
        }
        for (const auto &conn: connections) {
            conn->send(jsonStr);
        }
    }

    auto LogWebSocketManager::matches(const LogSubscription &subscription, const LogEntry &entry) -> bool {
        if (subscription.sessionId.has_value() && entry.sessionId != *subscription.sessionId) {
            return false;
        }
        if (subscription.level.has_value() && entry.level != *subscription.level) {
            return false;
        }
        if (!subscription.keyword.empty() && entry.source.find(subscription.keyword) == std::string::npos &&
            entry.content.find(subscription.keyword) == std::string::npos) {
            return false;
        }
        return true;
    }
} // namespace insoulforge
