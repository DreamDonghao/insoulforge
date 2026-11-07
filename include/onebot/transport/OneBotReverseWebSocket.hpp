/// @file OneBotReverseWebSocket.hpp
/// @brief OneBot 11 Universal 反向连接入口，使用 OneBot Access Token 认证。
#pragma once
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>

namespace insoulforge {
    class OneBotReverseAuthentication : public drogon::HttpFilter<OneBotReverseAuthentication> {
    public:
        void doFilter(const drogon::HttpRequestPtr &request, drogon::FilterCallback &&reject,
          drogon::FilterChainCallback &&next) override;
    };
    class OneBotReverseWebSocket : public drogon::WebSocketController<OneBotReverseWebSocket> {
    public:
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/onebot/ws", "insoulforge::OneBotReverseAuthentication");
        WS_PATH_LIST_END
        void handleNewConnection(
          const drogon::HttpRequestPtr &request, const drogon::WebSocketConnectionPtr &connection) override;
        void handleNewMessage(const drogon::WebSocketConnectionPtr &connection, std::string &&message,
          const drogon::WebSocketMessageType &type) override;
        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) override;
    };
} // namespace insoulforge
