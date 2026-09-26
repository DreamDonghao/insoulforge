/// @file OneBotWebSocketClient.cpp
/// @brief OneBot 正向 WebSocket 连接管理器实现

#include <infrastructure/NumericTypes.hpp>

#include <infrastructure/logging/Logger.hpp>
#include <onebot/transport/OneBotWebSocketClient.hpp>

#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/config/Config.hpp>

namespace insoulforge {
    namespace {
        constexpr f64 kReconnectDelaySeconds = 3.0;
    } // namespace

    auto OneBotWebSocketClient::instance() -> OneBotWebSocketClient & {
        static OneBotWebSocketClient client;
        return client;
    }

    OneBotWebSocketClient::ApiResponseAwaiter::ApiResponseAwaiter(
      OneBotWebSocketClient &client, std::string action, json params, const f64 timeout) :
        m_client(client), m_action(std::move(action)), m_params(std::move(params)), m_timeout(timeout) {}

    auto OneBotWebSocketClient::ApiResponseAwaiter::await_suspend(const std::coroutine_handle<> continuation) -> bool {
        if (m_client.sendApiRequest(std::move(m_action), std::move(m_params), m_timeout,
              [this, continuation](std::optional<json> response) mutable -> void {
                  setValue(std::move(response));
                  continuation.resume();
              })) {
            return true;
        }
        setValue(std::nullopt);
        return false;
    }

    void OneBotWebSocketClient::start() {
        u64 generation = 0;
        {
            std::scoped_lock lock(m_mutex);
            if (m_running || Config::instance().oneBotTransport != "websocket") {
                return;
            }
            m_running = true;
            generation = ++m_generation;
        }
        drogon::app().getLoop()->queueInLoop([this, generation] -> void { connect(generation); });
    }

    void OneBotWebSocketClient::stop() {
        drogon::WebSocketClientPtr client;
        std::vector<ResponseCallback> callbacks;
        {
            std::scoped_lock lock(m_mutex);
            ++m_generation;
            m_running = false;
            client = std::move(m_client);
            m_connection.reset();
            callbacks = takePendingCallbacksLocked();
        }
        if (client) {
            client->stop();
        }
        failRequests(callbacks);
    }

    void OneBotWebSocketClient::reconfigure() {
        stop();
        start();
    }

    auto OneBotWebSocketClient::isConnected() const -> bool {
        std::scoped_lock lock(m_mutex);
        return m_connection && m_connection->connected();
    }

    auto OneBotWebSocketClient::callApi(std::string action, json params, const f64 timeout)
      -> drogon::Task<std::optional<json>> {
        co_return co_await ApiResponseAwaiter(*this, std::move(action), std::move(params), timeout);
    }

    void OneBotWebSocketClient::connect(const u64 generation) {
        const auto &config = Config::instance();
        std::string origin;
        std::string path;
        {
            std::scoped_lock lock(m_mutex);
            if (!m_running || generation != m_generation || config.oneBotTransport != "websocket") {
                return;
            }
        }
        if (!splitWebSocketUrl(config.qqWebSocketHost, origin, path)) {
            Logger::error(0, "OneBot", fmt::format("OneBot WebSocket 地址无效: {}", config.qqWebSocketHost));
            return;
        }

        const auto client = drogon::WebSocketClient::newWebSocketClient(origin);
        const auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPathEncode(false);
        if (!config.accessToken.empty()) {
            // OneBot 实现通常支持请求头或 access_token 查询参数，二者同时提供以兼容常见实现。
            path += path.contains('?') ? "&" : "?";
            path += "access_token=" + drogon::utils::urlEncodeComponent(config.accessToken);
            request->addHeader("Authorization", "Bearer " + config.accessToken);
        }
        request->setPath(path);

        client->setMessageHandler(
          [this](std::string &&message, const drogon::WebSocketClientPtr &,
            const drogon::WebSocketMessageType &type) -> void { handleMessage(message, type); });
        client->setConnectionClosedHandler(
          [this](const drogon::WebSocketClientPtr &closedClient) -> void { handleConnectionClosed(closedClient); });

        {
            std::scoped_lock lock(m_mutex);
            if (!m_running || generation != m_generation) {
                client->stop();
                return;
            }
            m_client = client;
        }

        client->connectToServer(request,
          [this, generation](const drogon::ReqResult result, const drogon::HttpResponsePtr &,
            const drogon::WebSocketClientPtr &connectedClient) -> void {
              if (result != drogon::ReqResult::Ok) {
                  Logger::warn(
                    0, "OneBot", fmt::format("OneBot WebSocket 连接失败: result={}", static_cast<i32>(result)));
                  handleConnectionClosed(connectedClient);
                  return;
              }
              {
                  std::scoped_lock lock(m_mutex);
                  if (!m_running || generation != m_generation || m_client != connectedClient) {
                      connectedClient->stop();
                      return;
                  }
                  m_connection = connectedClient->getConnection();
              }
              Logger::info(0, "OneBot", fmt::format("OneBot WebSocket 已连接: {}", Config::instance().qqWebSocketHost));
          });
    }

    void OneBotWebSocketClient::scheduleReconnect(const u64 generation) {
        drogon::app().getLoop()->runAfter(kReconnectDelaySeconds, [this, generation] -> void { connect(generation); });
    }

    auto OneBotWebSocketClient::sendApiRequest(
      std::string action, json params, const f64 timeout, ResponseCallback callback) -> bool {
        drogon::WebSocketConnectionPtr connection;
        std::string echo;
        {
            std::scoped_lock lock(m_mutex);
            if (!m_running || !m_connection || !m_connection->connected()) {
                Logger::warn(0, "OneBot", fmt::format("OneBot WebSocket 未连接，无法调用动作: {}", action));
                return false;
            }
            echo = "insoulforge-" + std::to_string(++m_nextEcho);
            m_pendingRequests.emplace(echo, std::move(callback));
            connection = m_connection;
        }

        json request;
        request["action"] = std::move(action);
        request["params"] = std::move(params);
        request["echo"] = echo;
        try {
            connection->send(dumpJson(request));
        } catch (const std::exception &error) {
            Logger::warn(0, "OneBot", fmt::format("OneBot WebSocket 动作发送失败: {}", error.what()));
            std::scoped_lock lock(m_mutex);
            m_pendingRequests.erase(echo);
            return false;
        }
        drogon::app().getLoop()->runAfter(timeout, [this, echo] -> void { resolveRequest(echo, std::nullopt); });
        return true;
    }

    void OneBotWebSocketClient::handleMessage(const std::string &message, const drogon::WebSocketMessageType type) {
        if (type == drogon::WebSocketMessageType::Close) {
            const auto *data = reinterpret_cast<const unsigned char *>(message.data());
            const u16 closeCode = message.size() >= 2 ? static_cast<u16>((data[0] << 8U) | data[1]) : 0;
            const std::string reason = message.size() > 2 ? message.substr(2) : "";
            Logger::warn(
              0, "OneBot", fmt::format("OneBot WebSocket 收到关闭帧: code={}, reason={}", closeCode, reason));
            return;
        }
        if (type == drogon::WebSocketMessageType::Ping || type == drogon::WebSocketMessageType::Pong) {
            return;
        }
        if (type != drogon::WebSocketMessageType::Text) {
            Logger::warn(0, "OneBot", fmt::format("忽略 OneBot WebSocket 非文本消息"));
            return;
        }
        json body;
        if (!tryParseJson(message, body) || !body.is_object()) {
            Logger::warn(0, "OneBot", fmt::format("忽略 OneBot WebSocket 非法 JSON 消息"));
            return;
        }
        if (const std::string echo = jsonToString(atOrNull(body, "echo")); !echo.empty()) {
            resolveRequest(echo, std::move(body));
            return;
        }
        if (body.contains("post_type")) {
            OneBotEventWorkflow::instance().enqueueOneBotEvent(std::move(body));
        }
    }

    void OneBotWebSocketClient::handleConnectionClosed(const drogon::WebSocketClientPtr &client) {
        u64 generation = 0;
        bool shouldReconnect = false;
        std::vector<ResponseCallback> callbacks;
        {
            std::scoped_lock lock(m_mutex);
            if (m_client != client) {
                return;
            }
            m_client.reset();
            m_connection.reset();
            callbacks = takePendingCallbacksLocked();
            if (m_running && Config::instance().oneBotTransport == "websocket") {
                generation = m_generation;
                shouldReconnect = true;
            }
        }
        failRequests(callbacks);
        if (shouldReconnect) {
            Logger::warn(0, "OneBot", fmt::format("OneBot WebSocket 已断开，将在 {} 秒后重连", kReconnectDelaySeconds));
            scheduleReconnect(generation);
        }
    }

    void OneBotWebSocketClient::resolveRequest(const std::string &echo, std::optional<json> response) {
        ResponseCallback callback;
        {
            std::scoped_lock lock(m_mutex);
            const auto iter = m_pendingRequests.find(echo);
            if (iter == m_pendingRequests.end()) {
                return;
            }
            callback = std::move(iter->second);
            m_pendingRequests.erase(iter);
        }
        callback(std::move(response));
    }

    auto OneBotWebSocketClient::splitWebSocketUrl(const std::string &url, std::string &origin, std::string &path)
      -> bool {
        const size_t schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) {
            return false;
        }
        if (const std::string scheme = url.substr(0, schemeEnd); scheme != "ws" && scheme != "wss") {
            return false;
        }
        const size_t authorityStart = schemeEnd + 3;
        const size_t pathStart = url.find_first_of("/?#", authorityStart);
        if (pathStart == authorityStart) {
            return false;
        }
        origin = pathStart == std::string::npos ? url : url.substr(0, pathStart);
        path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
        return !origin.empty() && path.find('#') == std::string::npos;
    }

    auto OneBotWebSocketClient::takePendingCallbacksLocked() -> std::vector<OneBotWebSocketClient::ResponseCallback> {
        std::vector<ResponseCallback> callbacks;
        callbacks.reserve(m_pendingRequests.size());
        for (auto &callback: m_pendingRequests | std::views::values) {
            callbacks.emplace_back(std::move(callback));
        }
        m_pendingRequests.clear();
        return callbacks;
    }

    void OneBotWebSocketClient::failRequests(const std::vector<ResponseCallback> &callbacks) {
        for (auto &callback: callbacks) {
            callback(std::nullopt);
        }
    }
} // namespace insoulforge
