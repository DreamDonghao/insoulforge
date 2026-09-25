/// @file LogWebSocketManager.hpp
/// @brief 运行日志 WebSocket 管理器

#pragma once

#include <drogon/WebSocketConnection.h>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/LogBuffer.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace insoulforge {
    struct LogSubscription {
        std::optional<u64> sessionId;
        std::optional<std::string> level;
        std::string keyword;
    };

    class LogWebSocketManager {
    public:
        static auto instance() -> LogWebSocketManager &;

        void addConnection(const drogon::WebSocketConnectionPtr &conn);

        void removeConnection(const drogon::WebSocketConnectionPtr &conn);

        void updateSubscription(const drogon::WebSocketConnectionPtr &conn, LogSubscription subscription);

        /// @brief 向符合订阅条件的后台连接推送日志条目
        void pushLog(const LogEntry &entry);

        void broadcastStatus(const json &status);

    private:
        LogWebSocketManager() = default;

        [[nodiscard]] static auto matches(const LogSubscription &subscription, const LogEntry &entry) -> bool;

        std::mutex m_mutex;
        std::unordered_set<drogon::WebSocketConnectionPtr> m_connections;
        std::unordered_map<drogon::WebSocketConnectionPtr, LogSubscription> m_subscriptions;
    };
} // namespace insoulforge
