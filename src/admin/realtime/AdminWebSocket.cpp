#include <admin/realtime/AdminWebSocket.hpp>
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
        Logger::warn(0, "Admin", fmt::format("WebSocket消息解析失败"));
        return;
    }

    auto &wsMgr = WebSocketManager::instance();

    // 处理订阅请求（线上 JSON 字段沿用 "groupId"，字符串形式可承载大数，内部语义为 sessionId）
    if (msg.contains("action")) {
        if (std::string action = getStr(msg, "action"); action == "subscribe" && msg.contains("groupId")) {
            const uint64_t sessionId = parseUInt64(getStr(msg, "groupId"));
            wsMgr.subscribeSession(conn, sessionId);

            // 发送确认
            json resp;
            resp["type"] = "subscribed";
            resp["groupId"] = sessionId;
            conn->send(dumpJson(resp));
        } else if (action == "unsubscribe" && msg.contains("groupId")) {
            const uint64_t sessionId = parseUInt64(getStr(msg, "groupId"));
            wsMgr.unsubscribeSession(conn, sessionId);

            json resp;
            resp["type"] = "unsubscribed";
            resp["groupId"] = sessionId;
            conn->send(dumpJson(resp));
        }
    }
}

void AdminWebSocket::handleConnectionClosed(const WebSocketConnectionPtr &conn) {
    WebSocketManager::instance().removeConnection(conn);
}
