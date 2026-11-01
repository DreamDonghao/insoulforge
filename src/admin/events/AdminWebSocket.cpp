#include <admin/events/AdminWebSocket.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/logging/Logger.hpp>

using namespace insoulforge;
using namespace drogon;

void AdminWebSocket::handleNewConnection(const HttpRequestPtr &req, const WebSocketConnectionPtr &conn) {
    WebSocketManager::instance().addConnection(conn);

    // 发送欢迎消息
    json welcome;
    welcome["type"] = "connected";
    welcome["message"] = "WebSocket连接成功";
    conn->send(dumpJson(welcome));
}

void AdminWebSocket::handleNewMessage(
  const WebSocketConnectionPtr &conn, std::string &&message, const WebSocketMessageType &type) {
    if (type != WebSocketMessageType::Text) {
        return;
    }

    // 解析客户端消息
    json msg;
    if (!tryParseJson(message, msg)) {
        Logger::warn(std::nullopt, "Admin", fmt::format("WebSocket消息解析失败"));
        return;
    }

    auto &wsMgr = WebSocketManager::instance();

    // 处理订阅请求（线上 JSON 字段沿用 "sessionId"，字符串形式可承载大数，内部语义为 sessionId）
    if (msg.contains("action")) {
        if (std::string action = getStr(msg, "action"); action == "subscribe" && msg.contains("sessionId")) {
            const auto parsed = SessionId::parse(getStr(msg, "sessionId"));
            if (!parsed)
                return;
            const auto sessionId = *parsed;
            wsMgr.subscribeSession(conn, sessionId);

            // 发送确认
            json resp;
            resp["type"] = "subscribed";
            resp["sessionId"] = sessionId;
            conn->send(dumpJson(resp));
        } else if (action == "unsubscribe" && msg.contains("sessionId")) {
            const auto parsed = SessionId::parse(getStr(msg, "sessionId"));
            if (!parsed)
                return;
            const auto sessionId = *parsed;
            wsMgr.unsubscribeSession(conn, sessionId);

            json resp;
            resp["type"] = "unsubscribed";
            resp["sessionId"] = sessionId;
            conn->send(dumpJson(resp));
        }
    }
}

void AdminWebSocket::handleConnectionClosed(const WebSocketConnectionPtr &conn) {
    WebSocketManager::instance().removeConnection(conn);
}
