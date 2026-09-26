/// @file WebSocketManager.cpp
/// @brief 管理后台消息推送连接的实现

#include <admin/events/WebSocketManager.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    auto WebSocketManager::instance() -> WebSocketManager & {
        static WebSocketManager mgr;
        return mgr;
    }

    void WebSocketManager::addConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.insert(conn);
        Logger::info(0, "Admin", fmt::format("WebSocket连接已建立，当前连接数: {}", m_connections.size()));
    }

    void WebSocketManager::removeConnection(const drogon::WebSocketConnectionPtr &conn) {
        std::lock_guard lock(m_mutex);
        m_connections.erase(conn);
        // 清理订阅关系
        for (auto &subscribers: m_subscriptions | std::views::values) {
            subscribers.erase(conn);
        }
        Logger::info(0, "Admin", fmt::format("WebSocket连接已断开，当前连接数: {}", m_connections.size()));
    }

    void WebSocketManager::subscribeSession(const drogon::WebSocketConnectionPtr &conn, u64 sessionId) {
        std::lock_guard lock(m_mutex);
        m_subscriptions[sessionId].insert(conn);
        Logger::info(0, "Admin", fmt::format("WebSocket订阅会话: {}", sessionId));
    }

    void WebSocketManager::unsubscribeSession(const drogon::WebSocketConnectionPtr &conn, const u64 sessionId) {
        std::lock_guard lock(m_mutex);
        if (m_subscriptions.contains(sessionId)) {
            m_subscriptions[sessionId].erase(conn);
        }
    }

    void WebSocketManager::pushMessage(const u64 sessionId, const std::string &role, const std::string &content) {
        std::lock_guard lock(m_mutex);

        json msg;
        msg["type"] = "new_message";
        // 字符串形式：会话 ID 可能带私聊标志位，超出 JS Number 安全整数范围
        msg["groupId"] = std::to_string(sessionId);
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
