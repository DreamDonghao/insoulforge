/// @file LogWebSocket.hpp
/// @brief 运行日志 WebSocket 控制器

#pragma once

#include <drogon/WebSocketController.h>

namespace insoulforge {
    class LogWebSocket : public drogon::WebSocketController<LogWebSocket> {
    public:
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/admin/logs/ws");

        WS_PATH_LIST_END

        void handleNewConnection(
          const drogon::HttpRequestPtr &req, const drogon::WebSocketConnectionPtr &conn) override;

        void handleNewMessage(const drogon::WebSocketConnectionPtr &conn, std::string &&message,
          const drogon::WebSocketMessageType &type) override;

        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override;
    };
} // namespace insoulforge
