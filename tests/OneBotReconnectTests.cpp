/// @file OneBotReconnectTests.cpp
/// @brief 离线日志限频、底层警告过滤与真实 WebSocket 重连回归测试。
#include <atomic>
#include <cerrno>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/ConnectionRefusedLogFilter.hpp>
#include <infrastructure/logging/LogBuffer.hpp>
#include <onebot/transport/OneBotWebSocketClient.hpp>
#include <onebot/transport/ReconnectLogState.hpp>

namespace {
    using namespace insoulforge;
    using Clock = ReconnectLogState::Clock;
    using namespace std::chrono_literals;

    void require(const bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::atomic<bool> ready{false};
    std::atomic<int> attempts{0};
    std::mutex fixtureMutex;
    std::vector<Clock::time_point> attemptTimes;

    void testLogState() {
        ReconnectLogState state;
        const Clock::time_point start{};
        const auto first = state.record(2, start);
        require(first && first->first && first->failedAttempts == 1 && first->offlineSeconds == 0 && first->lastError == 2,
          "first failure is reported immediately");
        for (int second = 3; second < 300; second += 3)
            require(!state.record(2, start + std::chrono::seconds(second)), "retries must not produce repeated warnings");
        const auto summary = state.record(4, start + 300s);
        require(summary && !summary->first && summary->failedAttempts == 101 && summary->lastError == 4 &&
                  summary->offlineSeconds == 300, "five minute summary includes cumulative attempts and latest error");
        require(!state.record(2, start + 303s), "summary restarts the reporting window");
        const auto recovered = state.recover(start + 305s);
        require(recovered && recovered->failedAttempts == 102 && recovered->offlineSeconds == 305,
          "recovery reports final offline duration and failures");
        require(!state.recover(start + 306s), "recovery consumes the outage once");
        const auto disconnected = state.record(std::nullopt, start + 307s);
        require(disconnected && disconnected->first && disconnected->failedAttempts == 0 && !disconnected->lastError,
          "a new disconnect reports immediately without counting it as a failed attempt");
        require(!state.record(2, start + 310s), "first retry after disconnect does not duplicate the warning");
        state.reset();
        require(!state.recover(start + 311s) && state.record(2, start + 312s)->first,
          "intentional stop resets reporting state");
    }

    void testTrantorFilter() {
        ConnectionRefusedLogFilter filter;
        const Clock::time_point start{};
        const auto warning = "20261028 UTC 1 WARN  Connector::handleWrite - SO_ERROR = " +
          std::to_string(ECONNREFUSED) + " Connection refused - Connector.cc:181\n";
        require(filter.shouldWrite(warning, start), "first Connector warning is visible");
        require(!filter.shouldWrite(warning, start + 3s) && !filter.shouldWrite(warning, start + 299s),
          "duplicate Connector warnings are suppressed");
        require(filter.shouldWrite(warning, start + 300s), "Connector warning remains visible every five minutes");
        require(filter.shouldWrite("ERROR Connector::handleWrite - SO_ERROR = 61 Connection refused - Connector.cc:181", start),
          "error severity must not be filtered");
        require(filter.shouldWrite("WARN another connection refused", start), "other warnings remain visible");
        require(filter.shouldWrite("WARN Connector::handleWrite - SO_ERROR = 110 Timeout - Connector.cc:181", start),
          "other connection errors remain visible");
        require(filter.shouldWrite("WARN Connector::handleWrite - SO_ERROR = invalid - Connector.cc:181", start),
          "unknown log formats remain visible");
        filter.reset();
        require(filter.shouldWrite(warning, start + 301s), "reset makes the next first warning visible");
    }

    template<typename Predicate>
    auto waitFor(Predicate predicate, const char *description, const double seconds = 5.0) -> drogon::Task<> {
        const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
        while (!predicate()) {
            require(Clock::now() < deadline, description);
            co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.01);
        }
    }

    auto warnings() -> std::vector<LogEntry> {
        auto entries = LogBuffer::instance().query({.level = "warn", .limit = 100}).entries;
        std::erase_if(entries, [](const LogEntry &entry) { return entry.source != "OneBot"; });
        return entries;
    }

    auto testConnection(int &exitCode) -> drogon::Task<> {
        auto &client = OneBotWebSocketClient::instance();
        try {
            client.start();
            co_await waitFor([] { return attempts.load() >= 2; }, "timed out waiting for two failed attempts");
            require(!client.isConnected() && warnings().size() == 1, "offline retries log only one application warning");
            {
                std::scoped_lock lock(fixtureMutex);
                require(attemptTimes.size() == 2 && attemptTimes[1] - attemptTimes[0] >= 2500ms,
                  "only one three-second retry chain may run");
            }
            ready = true;
            co_await waitFor([&client] { return client.isConnected(); }, "timed out waiting for recovery");
            require(attempts.load() == 3, "availability is detected on the next normal retry");
            const auto response = co_await client.callApi("get_version", json::object(), 1.0);
            require(response && (*response)["status"] == "ok", "API calls work after recovery");
            const auto logs = LogBuffer::instance().query({.keyword = "已恢复连接", .limit = 100}).entries;
            require(logs.size() == 1 && logs.front().content.find("失败尝试 2 次") != std::string::npos,
              "recovery is reported once with the full attempt count");

            ready = false;
            const auto interrupted = co_await client.callApi("close_fixture", json::object(), 1.0);
            require(!interrupted, "disconnect must fail a pending API request");
            co_await waitFor([&client] { return !client.isConnected() && warnings().size() == 2; },
              "timed out waiting for disconnection and its warning");
            client.stop();
            const int stoppedAttempts = attempts.load();
            co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 3.2);
            require(attempts.load() == stoppedAttempts && warnings().size() == 2,
              "stop cancels stale retry timers without additional warnings");
            ready = true;
            client.reconfigure();
            co_await waitFor([&client] { return client.isConnected(); }, "timed out waiting for reconfigured connection");
            require(LogBuffer::instance().query({.keyword = "已恢复连接", .limit = 100}).entries.size() == 1,
              "reconfigure clears the previous offline state");
            exitCode = 0;
            std::cout << "OneBot reconnect tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        client.stop();
        drogon::app().quit();
    }
} // namespace

class ReconnectFixtureFilter : public drogon::HttpFilter<ReconnectFixtureFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr &, drogon::FilterCallback &&reject,
      drogon::FilterChainCallback &&next) override {
        const bool accept = ready.load();
        {
            std::scoped_lock lock(fixtureMutex);
            attemptTimes.push_back(Clock::now());
        }
        ++attempts;
        if (accept) {
            next();
        } else {
            const auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k503ServiceUnavailable);
            reject(response);
        }
    }
};

class ReconnectFixtureSocket : public drogon::WebSocketController<ReconnectFixtureSocket, false> {
public:
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/reconnect-fixture", "ReconnectFixtureFilter");
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr &, const drogon::WebSocketConnectionPtr &) override {}
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &) override {}
    void handleNewMessage(const drogon::WebSocketConnectionPtr &connection, std::string &&message,
      const drogon::WebSocketMessageType &type) override {
        if (type != drogon::WebSocketMessageType::Text) return;
        const auto request = insoulforge::json::parse(message);
        if (request["action"] == "close_fixture") {
            connection->forceClose();
            return;
        }
        connection->send(insoulforge::json{{"status", "ok"}, {"retcode", 0}, {"echo", request["echo"]}}.dump());
    }
};

auto main() -> int {
    try {
        testLogState();
        testTrantorFilter();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    drogon::app().registerFilter(std::make_shared<ReconnectFixtureFilter>());
    drogon::app().registerController(std::make_shared<ReconnectFixtureSocket>());
    drogon::app().addListener("127.0.0.1", 0).setThreadNum(1);
    trantor::EventLoopThread worker;
    worker.run();
    int exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        auto &config = insoulforge::Config::instance();
        config.oneBotTransport = "websocket";
        config.qqWebSocketHost = "ws://127.0.0.1:" +
          std::to_string(drogon::app().getListeners().front().toPort()) + "/reconnect-fixture";
        worker.getLoop()->queueInLoop([&] {
            drogon::async_run([&]() -> drogon::Task<> { co_await testConnection(exitCode); worker.getLoop()->quit(); });
        });
    });
    drogon::app().run();
    worker.wait();
    return exitCode;
}
