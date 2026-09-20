/// @file LogWebSocket.cpp
/// @brief 运行日志 WebSocket 控制器 - 实现

#include <admin/realtime/LogWebSocket.hpp>
#include <admin/realtime/LogWebSocketManager.hpp>

namespace insoulforge {
    void LogWebSocket::handleNewConnection(
      const drogon::HttpRequestPtr &req, const drogon::WebSocketConnectionPtr &conn) {
        LogWebSocketManager::instance().addConnection(conn);
        json welcome;
        welcome["type"] = "connected";
        welcome["message"] = "log websocket connected";
        conn->send(welcome.dump());
    }

    void LogWebSocket::handleNewMessage(
      const drogon::WebSocketConnectionPtr &conn, std::string &&message, const drogon::WebSocketMessageType &type) {
        if (type != drogon::WebSocketMessageType::Text) {
            return;
        }
        json msg;
        if (!tryParseJson(message, msg)) {
            spdlog::warn("日志WebSocket消息解析失败");
            return;
        }

        if (getStr(msg, "action") == "subscribe") {
            // 线上 JSON 字段沿用 "groupId"（内部语义为 sessionId）
            LogSubscription sub;
            const json &groupIdVal = atOrNull(msg, "groupId");
            sub.all = groupIdVal.is_null() || (groupIdVal.is_string() && groupIdVal.get<std::string>() == "all");
            if (groupIdVal.is_string() && groupIdVal.get<std::string>() == "system") {
                sub.all = false;
                sub.systemOnly = true;
            } else if (!sub.all) {
                // sessionId 由前端以字符串形式发送（群号可能超过 JS 安全整数范围），需安全解析
                sub.sessionId = jsonToUInt64(groupIdVal);
            }
            if (atOrNull(msg, "level").is_string() && getStr(msg, "level") != "all") {
                sub.level = getStr(msg, "level");
            }
            if (msg.contains("keyword") && atOrNull(msg, "keyword").is_string()) {
                sub.keyword = getStr(msg, "keyword");
            }
            LogWebSocketManager::instance().updateSubscription(conn, std::move(sub));
        }
    }

    void LogWebSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) {
        LogWebSocketManager::instance().removeConnection(conn);
    }
} // namespace insoulforge
