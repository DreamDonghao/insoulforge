/// @file WebSocketManager.cpp
/// @brief 管理后台消息推送连接的实现

#include <admin/events/WebSocketManager.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    auto WebSocketManager::instance() -> WebSocketManager & {
        static WebSocketManager mgr;
        return mgr;
    }

    void WebSocketManager::addConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.insert(conn);
        Logger::info(std::nullopt, "Admin", fmt::format("WebSocket连接已建立，当前连接数: {}", m_connections.size()));
    }

    void WebSocketManager::removeConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.erase(conn);
        // 清理订阅关系
        for (auto &subscribers: m_subscriptions | std::views::values) {
            subscribers.erase(conn);
        }
        Logger::info(std::nullopt, "Admin", fmt::format("WebSocket连接已断开，当前连接数: {}", m_connections.size()));
    }

    void WebSocketManager::subscribeSession(const drogon::WebSocketConnectionPtr &conn, const SessionId &sessionId) {
        std::lock_guard lock(m_mutex);
        m_subscriptions[sessionId].insert(conn);
        Logger::info(std::nullopt, "Admin", fmt::format("WebSocket订阅会话: {}", sessionId));
    }

    void WebSocketManager::unsubscribeSession(const drogon::WebSocketConnectionPtr &conn, const SessionId &sessionId) {
        std::lock_guard lock(m_mutex);
        if (m_subscriptions.contains(sessionId)) {
            m_subscriptions[sessionId].erase(conn);
        }
    }

    void WebSocketManager::pushMessage(
      const SessionId &sessionId, const std::string &role, const std::string &content) {
        std::lock_guard lock(m_mutex);

        json msg;
        msg["type"] = "new_message";
        // 会话 ID 使用带类型的规范字符串
        msg["sessionId"] = sessionId.toString();
        msg["data"]["role"] = role;
        msg["data"]["content"] = content;
        msg["data"]["timestamp"] = currentDateTime();
        const std::string jsonStr = dumpJson(msg);

        // 先通知订阅当前会话的连接。
        if (m_subscriptions.contains(sessionId)) {
            for (const auto &conn: m_subscriptions[sessionId]) {
                conn->send(jsonStr);
            }
        }

        // 未设置任何会话订阅的连接接收全部消息。
        for (const auto &conn: m_connections) {
            bool hasSubscription = false;
            for (const auto &subscribers: m_subscriptions | std::views::values) {
                if (subscribers.contains(conn)) {
                    hasSubscription = true;
                    break;
                }
            }
            if (!hasSubscription) {
                conn->send(jsonStr);
            }
        }
    }

    void WebSocketManager::broadcastEvent(const std::string &type, const json &data) const {
        std::lock_guard lock(m_mutex);

        json msg;
        msg["type"] = type;
        msg["data"] = data;
        const std::string jsonStr = dumpJson(msg);

        for (const auto &conn: m_connections) {
            conn->send(jsonStr);
        }
    }
} // namespace insoulforge
