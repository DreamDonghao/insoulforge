/// @file LogWebSocketManager.hpp
/// @brief 运行日志 WebSocket 管理器

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <drogon/WebSocketConnection.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/LogBuffer.hpp>

namespace insoulforge {
    /// @brief 一个日志连接的过滤条件；空值表示不过滤该字段
    struct LogSubscription {
        std::optional<u64> sessionId;
        std::optional<std::string> level;
        std::string keyword;
    };

    /// @brief 管理后台日志连接及其订阅条件
    /// @details 只有提交过订阅条件的连接会收到实时日志；状态事件会广播给所有连接。
    class LogWebSocketManager {
    public:
        static auto instance() -> LogWebSocketManager &;

        void addConnection(const drogon::WebSocketConnectionPtr &conn);

        void removeConnection(const drogon::WebSocketConnectionPtr &conn);

        /// @brief 覆盖指定连接的过滤条件
        void updateSubscription(const drogon::WebSocketConnectionPtr &conn, LogSubscription subscription);

        /// @brief 向符合订阅条件的后台连接推送日志条目
        void pushLog(const LogEntry &entry);

        /// @brief 向所有日志连接广播运行状态
        void broadcastStatus(const json &status);

    private:
        LogWebSocketManager() = default;

        [[nodiscard]] static auto matches(const LogSubscription &subscription, const LogEntry &entry) -> bool;

        std::mutex m_mutex;
        std::unordered_set<drogon::WebSocketConnectionPtr> m_connections;
        std::unordered_map<drogon::WebSocketConnectionPtr, LogSubscription> m_subscriptions;
    };
} // namespace insoulforge
