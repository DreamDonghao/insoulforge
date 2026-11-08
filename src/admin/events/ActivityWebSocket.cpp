/// @file ActivityWebSocket.cpp
/// @brief 握手校验统一接入 Token，服务通过 register 绑定名称，协议处理交由 ActivityManager。
#include <admin/events/ActivityWebSocket.hpp>
#include <workflow/activity/ActivityManager.hpp>

namespace insoulforge {
    void ActivityAuthentication::doFilter(
      const drogon::HttpRequestPtr &request, drogon::FilterCallback &&reject, drogon::FilterChainCallback &&next) {
        const auto authorization = request->getHeader("authorization");
        if (authorization.starts_with("Bearer ") && ActivityManager::instance().authorized(authorization.substr(7))) {
            next();
            return;
        }
        const auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k401Unauthorized);
        response->addHeader("Cache-Control", "no-store");
        reject(response);
    }
    void ActivityWebSocket::handleNewConnection(
      const drogon::HttpRequestPtr &request, const drogon::WebSocketConnectionPtr &connection) {
        if (!ActivityManager::instance().connect(request->getHeader("authorization").substr(7), connection)) {
            connection->shutdown();
        }
    }
    void ActivityWebSocket::handleNewMessage(const drogon::WebSocketConnectionPtr &connection, std::string &&message,
      const drogon::WebSocketMessageType &type) {
        ActivityManager::instance().receive(connection, message, type);
    }
    void ActivityWebSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) {
        ActivityManager::instance().disconnect(connection);
    }
} // namespace insoulforge
