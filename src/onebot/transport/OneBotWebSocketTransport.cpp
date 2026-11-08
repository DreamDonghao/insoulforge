/// @file OneBotWebSocketTransport.cpp
/// @brief OneBot 正向 WebSocket 连接管理器实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/transport/OneBotWebSocketTransport.hpp>
#include <workflow/EventWorkflow.hpp>

namespace insoulforge {
    namespace {
        constexpr f64 kReconnectDelaySeconds = 3.0;
    } // namespace

    auto OneBotWebSocketTransport::instance() -> OneBotWebSocketTransport & {
        static OneBotWebSocketTransport client;
        return client;
    }

    OneBotWebSocketTransport::ApiResponseAwaiter::ApiResponseAwaiter(
      OneBotWebSocketTransport &client, std::string action, json params, const f64 timeout, json expectedConnection) :
        m_client(client), m_action(std::move(action)), m_params(std::move(params)), m_timeout(timeout),
        m_expectedConnection(std::move(expectedConnection)) {}

    auto OneBotWebSocketTransport::ApiResponseAwaiter::await_suspend(const std::coroutine_handle<> continuation)
      -> bool {
        if (m_client.sendApiRequest(
              std::move(m_action), std::move(m_params), m_timeout,
              [this, continuation](std::optional<json> response) mutable -> void {
                  setValue(std::move(response));
                  continuation.resume();
              },
              m_expectedConnection)) {
            return true;
        }
        setValue(std::nullopt);
        return false;
    }

    void OneBotWebSocketTransport::acceptReverse(drogon::WebSocketConnectionPtr connection, json config) {
        drogon::app().getLoop()->queueInLoop([this, connection = std::move(connection), config = std::move(config)] {
            if (config != Config::instance().oneBotSnapshot() || !connection->connected()) {
                connection->shutdown();
                return;
            }
            stop();
            {
                std::scoped_lock lock(m_mutex);
                if (getStr(config, "oneBotTransport") != "reverse_websocket" ||
                    config != Config::instance().oneBotSnapshot()) {
                    connection->shutdown();
                    return;
                }
                m_running = true;
                m_connectionConfig = config;
                m_connection = connection;
                m_lastReceive = std::chrono::steady_clock::now();
                const auto generation = m_generation;
                drogon::app().getLoop()->runAfter(30, [this, generation] { checkReverseTimeout(generation); });
            }
            connection->setPingMessage("onebot", std::chrono::seconds(30));
            Logger::info(std::nullopt, "OneBot", "OneBot 反向 WebSocket 已连接");
        });
    }

    void OneBotWebSocketTransport::receiveReverse(const drogon::WebSocketConnectionPtr &connection,
      const std::string &message, const drogon::WebSocketMessageType type) {
        {
            std::scoped_lock lock(m_mutex);
            if (m_connection != connection)
                return;
            m_lastReceive = std::chrono::steady_clock::now();
        }
        handleMessage(message, type);
    }

    void OneBotWebSocketTransport::closeReverse(const drogon::WebSocketConnectionPtr &connection) {
        std::vector<ResponseCallback> callbacks;
        {
            std::scoped_lock lock(m_mutex);
            if (m_connection != connection)
                return;
            m_connection.reset();
            m_running = false;
            callbacks = takePendingCallbacksLocked();
        }
        failRequests(callbacks);
        Logger::info(std::nullopt, "OneBot", "OneBot 反向 WebSocket 已断开，等待重新接入");
    }

    void OneBotWebSocketTransport::checkReverseTimeout(const u64 generation) {
        drogon::WebSocketConnectionPtr expired;
        {
            std::scoped_lock lock(m_mutex);
            if (generation != m_generation || !m_connection || m_client)
                return;
            if (std::chrono::steady_clock::now() - m_lastReceive >= std::chrono::seconds(90))
                expired = m_connection;
            else
                drogon::app().getLoop()->runAfter(30, [this, generation] { checkReverseTimeout(generation); });
        }
        if (expired) {
            closeReverse(expired);
            expired->forceClose();
        }
    }

    void OneBotWebSocketTransport::start() {
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

    void OneBotWebSocketTransport::stop() {
        drogon::WebSocketClientPtr client;
        drogon::WebSocketConnectionPtr connection;
        std::vector<ResponseCallback> callbacks;
        {
            std::scoped_lock lock(m_mutex);
            ++m_generation;
            m_running = false;
            m_reconnectLog.reset();
            client = std::move(m_client);
            connection = std::move(m_connection);
            callbacks = takePendingCallbacksLocked();
        }
        if (connection)
            connection->shutdown();
        if (client) {
            // 域名连接持有 DNS 解析器，停止和最终析构都必须回到所属事件循环。
            // 转移所有权，避免循环执行完回调后，最后一份引用仍在调用线程释放。
            auto *loop = client->getLoop();
            loop->runInLoop([client = std::move(client)] { client->stop(); });
        }
        failRequests(callbacks);
        Logger::resetConnectionRefusedWarnings();
    }

    void OneBotWebSocketTransport::reconfigure() {
        stop();
        start();
    }

    auto OneBotWebSocketTransport::isConnected() const -> bool {
        std::scoped_lock lock(m_mutex);
        return m_connection && m_connection->connected();
    }

    auto OneBotWebSocketTransport::callApi(std::string action, json params, const f64 timeout, json expectedConnection)
      -> drogon::Task<std::optional<json>> {
        co_return co_await ApiResponseAwaiter(
          *this, std::move(action), std::move(params), timeout, std::move(expectedConnection));
    }

    void OneBotWebSocketTransport::connect(const u64 generation) {
        const auto connectionConfig = Config::instance().oneBotSnapshot();
        std::string origin;
        std::string path;
        {
            std::scoped_lock lock(m_mutex);
            if (!m_running || generation != m_generation ||
                getStr(connectionConfig, "oneBotTransport") != "websocket") {
                return;
            }
        }
        if (!splitWebSocketUrl(getStr(connectionConfig, "qqWebSocketHost"), origin, path)) {
            Logger::error(std::nullopt, "OneBot",
              fmt::format("OneBot WebSocket 地址无效: {}", getStr(connectionConfig, "qqWebSocketHost")));
            return;
        }

        const auto client = drogon::WebSocketClient::newWebSocketClient(origin);
        const auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPathEncode(false);
        if (!getStr(connectionConfig, "accessToken").empty()) {
            // OneBot 实现通常支持请求头或 access_token 查询参数，二者同时提供以兼容常见实现。
            path += path.contains('?') ? "&" : "?";
            path += "access_token=" + drogon::utils::urlEncodeComponent(getStr(connectionConfig, "accessToken"));
            request->addHeader("Authorization", "Bearer " + getStr(connectionConfig, "accessToken"));
        }
        request->setPath(path);

        client->setMessageHandler([this, generation](std::string &&message, const drogon::WebSocketClientPtr &source,
                                    const drogon::WebSocketMessageType &type) -> void {
            {
                std::scoped_lock lock(m_mutex);
                if (!m_running || generation != m_generation || m_client != source)
                    return;
            }
            handleMessage(message, type);
        });
        client->setConnectionClosedHandler(
          [this](const drogon::WebSocketClientPtr &closedClient) -> void { handleConnectionClosed(closedClient); });

        bool current = false;
        {
            std::scoped_lock lock(m_mutex);
            if (m_running && generation == m_generation) {
                m_client = client;
                current = true;
            }
        }
        if (!current) {
            client->stop();
            return;
        }

        client->connectToServer(request,
          [this, generation, connectionConfig](const drogon::ReqResult result, const drogon::HttpResponsePtr &,
            const drogon::WebSocketClientPtr &connectedClient) -> void {
              if (result != drogon::ReqResult::Ok) {
                  handleConnectionClosed(connectedClient, result);
                  return;
              }
              std::optional<ReconnectLogState::Report> recovered;
              bool accepted = false;
              {
                  std::scoped_lock lock(m_mutex);
                  if (m_running && generation == m_generation && m_client == connectedClient) {
                      m_connectionConfig = connectionConfig;
                      m_connection = connectedClient->getConnection();
                      recovered = m_reconnectLog.recover();
                      accepted = true;
                  }
              }
              if (!accepted) {
                  connectedClient->stop();
                  return;
              }
              Logger::resetConnectionRefusedWarnings();
              if (recovered) {
                  Logger::info(std::nullopt, "OneBot",
                    fmt::format("OneBot WebSocket 已恢复连接，离线 {} 秒，失败尝试 {} 次", recovered->offlineSeconds,
                      recovered->failedAttempts));
              } else {
                  Logger::info(std::nullopt, "OneBot",
                    fmt::format("OneBot WebSocket 已连接: {}", Config::instance().qqWebSocketHost));
              }
          });
    }

    void OneBotWebSocketTransport::scheduleReconnect(const u64 generation) {
        drogon::app().getLoop()->runAfter(kReconnectDelaySeconds, [this, generation] -> void { connect(generation); });
    }

    auto OneBotWebSocketTransport::sendApiRequest(std::string action, json params, const f64 timeout,
      ResponseCallback callback, const json &expectedConnection) -> bool {
        const bool currentConfig =
          expectedConnection.is_null() || expectedConnection == Config::instance().oneBotSnapshot();
        drogon::WebSocketConnectionPtr connection;
        std::string echo;
        {
            std::scoped_lock lock(m_mutex);
            bool matchesConnection = currentConfig;
            if (!expectedConnection.is_null()) {
                for (const auto *key:
                  {"oneBotImplementation", "oneBotTransport", "qqWebSocketHost", "accessToken", "selfQQNumber"})
                    matchesConnection =
                      matchesConnection && atOrNull(expectedConnection, key) == atOrNull(m_connectionConfig, key);
            }
            if (!matchesConnection || !m_running || !m_connection || !m_connection->connected()) {
                Logger::warn(std::nullopt, "OneBot", fmt::format("OneBot WebSocket 未连接，无法调用动作: {}", action));
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
            Logger::warn(std::nullopt, "OneBot", fmt::format("OneBot WebSocket 动作发送失败: {}", error.what()));
            std::scoped_lock lock(m_mutex);
            m_pendingRequests.erase(echo);
            return false;
        }
        drogon::app().getLoop()->runAfter(timeout, [this, echo] -> void { resolveRequest(echo, std::nullopt); });
        return true;
    }

    void OneBotWebSocketTransport::handleMessage(const std::string &message, const drogon::WebSocketMessageType type) {
        if (type == drogon::WebSocketMessageType::Close) {
            const auto *data = reinterpret_cast<const unsigned char *>(message.data());
            const u16 closeCode = message.size() >= 2 ? static_cast<u16>((data[0] << 8U) | data[1]) : 0;
            const std::string reason = message.size() > 2 ? message.substr(2) : "";
            Logger::warn(std::nullopt, "OneBot",
              fmt::format("OneBot WebSocket 收到关闭帧: code={}, reason={}", closeCode, reason));
            return;
        }
        if (type == drogon::WebSocketMessageType::Ping || type == drogon::WebSocketMessageType::Pong) {
            return;
        }
        if (type != drogon::WebSocketMessageType::Text) {
            Logger::warn(std::nullopt, "OneBot", fmt::format("忽略 OneBot WebSocket 非文本消息"));
            return;
        }
        json body;
        if (!tryParseJson(message, body) || !body.is_object()) {
            Logger::warn(std::nullopt, "OneBot", fmt::format("忽略 OneBot WebSocket 非法 JSON 消息"));
            return;
        }
        if (const std::string echo = jsonToString(atOrNull(body, "echo")); !echo.empty()) {
            resolveRequest(echo, std::move(body));
            return;
        }
        if (body.contains("post_type")) {
            EventWorkflow::instance().enqueueOneBotEvent(body);
        }
    }

    void OneBotWebSocketTransport::handleConnectionClosed(
      const drogon::WebSocketClientPtr &client, const std::optional<drogon::ReqResult> failure) {
        u64 generation = 0;
        bool shouldReconnect = false;
        std::optional<ReconnectLogState::Report> report;
        std::vector<ResponseCallback> callbacks;
        {
            std::scoped_lock lock(m_mutex);
            if (!m_running || m_client != client) {
                return;
            }
            m_client.reset();
            m_connection.reset();
            callbacks = takePendingCallbacksLocked();
            if (m_running && Config::instance().oneBotTransport == "websocket") {
                generation = m_generation;
                shouldReconnect = true;
                report = m_reconnectLog.record(failure ? std::optional{static_cast<i32>(*failure)} : std::nullopt);
            }
        }
        // 统一由本类安排 3 秒后的重连，防止 Drogon 客户端的自动重试形成第二条重连链。
        client->stop();
        failRequests(callbacks);
        if (shouldReconnect) {
            if (report) {
                const auto error = report->lastError ? fmt::format("result={}", *report->lastError) : "连接已断开";
                if (report->first) {
                    Logger::warn(std::nullopt, "OneBot",
                      fmt::format("OneBot WebSocket 连接不可用: {}，每 {} 秒重试，持续失败每 5 分钟汇总", error,
                        kReconnectDelaySeconds));
                } else {
                    Logger::warn(std::nullopt, "OneBot",
                      fmt::format(
                        "OneBot WebSocket 仍未连接，已离线 {} 秒，失败尝试 {} 次，最近错误: {}，继续每 {} 秒重试",
                        report->offlineSeconds, report->failedAttempts, error, kReconnectDelaySeconds));
                }
            }
            scheduleReconnect(generation);
        }
    }

    void OneBotWebSocketTransport::resolveRequest(const std::string &echo, std::optional<json> response) {
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

    auto OneBotWebSocketTransport::splitWebSocketUrl(const std::string &url, std::string &origin, std::string &path)
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

    auto OneBotWebSocketTransport::takePendingCallbacksLocked() -> std::vector<ResponseCallback> {
        std::vector<ResponseCallback> callbacks;
        callbacks.reserve(m_pendingRequests.size());
        for (auto &callback: m_pendingRequests | std::views::values) {
            callbacks.emplace_back(std::move(callback));
        }
        m_pendingRequests.clear();
        return callbacks;
    }

    void OneBotWebSocketTransport::failRequests(const std::vector<ResponseCallback> &callbacks) {
        for (auto &callback: callbacks) {
            callback(std::nullopt);
        }
    }
} // namespace insoulforge
