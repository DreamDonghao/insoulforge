#include <infrastructure/config/Config.hpp>
#include <onebot/transport/OneBotReverseWebSocket.hpp>
#include <onebot/transport/OneBotWebSocketTransport.hpp>

namespace insoulforge {
    namespace {
        auto authorized(const drogon::HttpRequestPtr &request, const json &config) -> bool {
            const auto token = getStr(config, "accessToken");
            return getStr(config, "oneBotTransport") == "reverse_websocket" && !token.empty() &&
                   request->getHeader("authorization") == "Bearer " + token &&
                   request->getHeader("x-client-role") == "Universal" &&
                   request->getHeader("x-self-id") == jsonToString(atOrNull(config, "selfQQNumber"));
        }
    } // namespace
    void OneBotReverseAuthentication::doFilter(
      const drogon::HttpRequestPtr &request, drogon::FilterCallback &&reject, drogon::FilterChainCallback &&next) {
        if (authorized(request, Config::instance().oneBotSnapshot())) {
            next();
            return;
        }
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k401Unauthorized);
        reject(response);
    }
    void OneBotReverseWebSocket::handleNewConnection(
      const drogon::HttpRequestPtr &request, const drogon::WebSocketConnectionPtr &connection) {
        const auto config = Config::instance().oneBotSnapshot();
        if (!authorized(request, config)) {
            connection->shutdown();
            return;
        }
        OneBotWebSocketTransport::instance().acceptReverse(connection, config);
    }
    void OneBotReverseWebSocket::handleNewMessage(const drogon::WebSocketConnectionPtr &connection,
      std::string &&message, const drogon::WebSocketMessageType &type) {
        OneBotWebSocketTransport::instance().receiveReverse(connection, message, type);
    }
    void OneBotReverseWebSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) {
        OneBotWebSocketTransport::instance().closeReverse(connection);
    }
} // namespace insoulforge
