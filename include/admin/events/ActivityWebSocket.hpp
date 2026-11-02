/// @file ActivityWebSocket.hpp
/// @brief 独立活动服务的反向连接入口，与管理员 WebSocket 使用不同凭据。
#pragma once
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>

namespace insoulforge {
    class ActivityAuthentication : public drogon::HttpFilter<ActivityAuthentication> {
    public:
        void doFilter(const drogon::HttpRequestPtr &request, drogon::FilterCallback &&reject,
          drogon::FilterChainCallback &&next) override;
    };
    class ActivityWebSocket : public drogon::WebSocketController<ActivityWebSocket> {
    public:
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/activities/ws", "insoulforge::ActivityAuthentication");
        WS_PATH_LIST_END
        void handleNewConnection(
          const drogon::HttpRequestPtr &request, const drogon::WebSocketConnectionPtr &connection) override;
        void handleNewMessage(const drogon::WebSocketConnectionPtr &connection, std::string &&message,
          const drogon::WebSocketMessageType &type) override;
        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) override;
    };
} // namespace insoulforge
