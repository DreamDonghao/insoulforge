/// @file ActivityTests.cpp
/// @brief 使用本地模拟服务验证全局活动协议，不启动机器人或访问外部服务。
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <admin/http/ActivityController.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/activity/ActivityManager.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/plugins/ActivityToolsPlugin.hpp>
#include <workflow/session/SessionConfigManager.hpp>
#include <workflow/session/SessionStore.hpp>

namespace {
    using namespace insoulforge;
    constexpr auto kToken = "activity-fixture-secret";
    auto fixtureMutex = std::mutex{};
    auto remote = json{{"running", false}};
    auto calls = std::map<std::string, int>{};
    auto lastCommand = json{};
    bool unavailable = false;
    bool loseStartResponse = false;
    bool delayStart = false;
    std::atomic<bool> startReceived{false};
    std::atomic<int> executorRequests{0};
    std::atomic<int> routerRequests{0};
    std::atomic<int> deliveries{0};
    std::atomic<int> concurrentExecutors{0};
    std::atomic<int> maxConcurrentExecutors{0};
    json lastModelContext;

    void require(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    auto configuration(const std::string &url) -> json {
        return {{"callbackUrl", "http://127.0.0.1/activities/events"},
          {"servers", json::array({{{"name", "chess"}, {"description", "一起下棋"}, {"url", url}, {"enabled", true},
                        {"token", kToken}}})}};
    }
    auto origin(const std::string &session = "group:test-a", const std::string &user = "user-a") -> json {
        return {{"session_id", session}, {"user_id", user}};
    }
    auto changeSnapshot(const std::string &status, bool running = true) -> json {
        std::lock_guard lock(fixtureMutex);
        remote["sequence"] = remote.value("sequence", 0) + 1;
        remote["status"] = status;
        remote["running"] = running;
        auto result = remote;
        result["name"] = "chess";
        return result;
    }
    template<typename Predicate>
    auto waitFor(Predicate predicate, const char *message) -> drogon::Task<> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate()) {
            require(std::chrono::steady_clock::now() < deadline, message);
            co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.01);
        }
    }
    auto testWorkflow(std::string endpoint, std::filesystem::path root) -> drogon::Task<> {
        auto &config = Config::instance();
        auto &database = Database::instance();
        require(database.initialize((root / "fixture.db").string()).has_value(), "workflow fixture database failed");
        for (const auto *role: {"router", "executor"}) {
            auto settings = config.getLLMConfig(role);
            settings["baseUrl"] = endpoint;
            settings["path"] = "/activity-model";
            settings["model"] = role;
            settings["maxRetries"] = 0;
            require(config.saveLLMConfig(role, settings).has_value(), "model fixture configuration failed");
        }
        auto qq = config.getQQConfig();
        qq["oneBotTransport"] = "http";
        qq["qqHttpHost"] = endpoint;
        require(config.saveQQConfig(qq).has_value(), "OneBot fixture configuration failed");
        auto &agent = AgentSystem::instance();
        require(agent.initialize(database, "fixture").has_value(), "workflow fixture agent failed");
        auto &workflow = OneBotEventWorkflow::instance();
        require(workflow.initialize(database, config, agent).has_value(), "workflow initialization failed");
        const auto session = SessionId::group("12345");
        SessionStore::enableSession(session);
        workflow.enqueueActivityMessage(session, "静默", "chess", "silent", origin());
        co_await waitFor(
          [&] { return SessionConfigManager::getConfig(session).allMesCount == 1; }, "silent not recorded");
        require(executorRequests == 0 && routerRequests == 0, "silent event invoked model");
        workflow.enqueueActivityMessage(session, "判断", "chess", "judge", origin());
        co_await waitFor(
          [&] { return routerRequests == 1 && SessionConfigManager::getConfig(session).allMesCount == 2; },
          "judge not routed");
        require(executorRequests == 0, "judge ignored skip decision");
        workflow.enqueueActivityMessage(session, "强制", "chess", "force", origin());
        co_await waitFor([&] { return executorRequests == 1; }, "force not delivered to executor");
        workflow.enqueueActivityMessage(session, "继续", "chess", "force", origin());
        co_await waitFor([&] { return deliveries == 2; }, "queued force events lost");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(routerRequests == 1 && maxConcurrentExecutors == 1, "force routed or replies ran concurrently");
        {
            std::lock_guard lock(fixtureMutex);
            require(lastModelContext.contains("running_activities"), "model context omitted activities");
            auto keys = std::vector<std::string>{};
            for (const auto &[key, value]: lastModelContext.items())
                keys.push_back(key);
            require(keys.back() == "running_activities", "activities must be the last context field");
            require(
              std::ranges::any_of(lastModelContext["recent_messages"],
                [](const json &message) { return atOrNull(atOrNull(message, "activity"), "origin") == origin(); }),
              "activity origin not projected to model");
        }
        const auto privateSession = SessionId::privateChat(UserId{"67890"});
        SessionStore::enableSession(privateSession);
        workflow.enqueueActivityMessage(privateSession, "私聊", "chess", "force", origin());
        co_await waitFor([&] { return deliveries == 3; }, "private event not delivered");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        {
            std::lock_guard lock(fixtureMutex);
            require(
              !lastModelContext.contains("response_requirements"), "activity added reply strategy to private chat");
        }
        agent.setRunning(false);
        workflow.enqueueActivityMessage(session, "禁用", "chess", "force", origin());
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(deliveries == 3, "force bypassed bot run switch");
        agent.setRunning(true);
        SessionStore::disableSession(session);
        workflow.enqueueActivityMessage(session, "禁用", "chess", "force", origin());
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(deliveries == 3, "force bypassed session switch");
        workflow.flushMessageListsToStorage();
        database.close();
    }
    auto run(const std::string endpoint, const std::filesystem::path root, int &exitCode) -> drogon::Task<> {
        try {
            auto events = std::vector<ActivityManager::Event>{};
            auto eventMutex = std::mutex{};
            ActivityManager manager((root / "state.json").string(), [&](ActivityManager::Event event) {
                std::lock_guard lock(eventMutex);
                events.push_back(std::move(event));
            });
            auto config = configuration(endpoint);
            require(manager.initialize(config).has_value(), "valid config rejected");
            auto invalid = config;
            invalid["servers"].push_back(invalid["servers"][0]);
            require(!manager.initialize(invalid), "duplicate names accepted");
            invalid = config;
            invalid["servers"][0]["token"] = "short";
            require(!manager.initialize(invalid), "weak or missing callback key accepted");
            require(
              manager.directory().size() == 1 && manager.runningActivities().empty(), "initial directory invalid");
            require(manager.configuration()["servers"][0]["token"] == kToken,
              "admin editor did not receive configured secret");
            require(manager.directory().dump().find(kToken) == std::string::npos &&
                      manager.runningActivities().dump().find(kToken) == std::string::npos,
              "secret leaked to model context");
            const auto beforeSync = co_await manager.invoke("start", "chess", json::object(), origin());
            require(beforeSync.value("success", true) == false, "start before recovery accepted");
            co_await manager.synchronize("chess");
            {
                std::lock_guard lock(fixtureMutex);
                delayStart = true;
            }
            std::atomic<bool> firstDone{false};
            json firstResult;
            drogon::async_run([&]() -> drogon::Task<> {
                firstResult = co_await manager.invoke("start", "chess", json::object(), origin());
                firstDone = true;
            });
            while (!startReceived)
                co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.01);
            const auto duplicate = co_await manager.invoke("start", "chess", json::object(), origin("group:test-b"));
            require(duplicate.value("success", true) == false, "concurrent global duplicate start accepted");
            while (!firstDone)
                co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.01);
            require(firstResult["url"] == endpoint + "/play", "participation link lost");
            require(manager.runningActivities().size() == 1, "started activity absent from context");
            {
                std::lock_guard lock(fixtureMutex);
                require(calls["start"] == 1, "duplicate start reached service");
                delayStart = false;
            }
            auto event = changeSnapshot("等待用户落子");
            event["text"] = "轮到你了";
            event["mode"] = "judge";
            require(!manager.receiveEvent("chess", "wrong", event), "invalid callback auth accepted");
            require(manager.receiveEvent("chess", kToken, event).has_value(), "valid callback rejected");
            require(
              manager.receiveEvent("chess", kToken, event).has_value(), "duplicate callback should be acknowledged");
            require(events.size() == 1 && events.back().sessionId == SessionId::group("test-a") &&
                      events.back().mode == "judge",
              "notification lost or duplicated/default target wrong");
            require(manager.runningActivities()[0]["status"] == "等待用户落子", "pushed status not visible");

            auto older = event;
            older["sequence"] = 0;
            older["status"] = "旧状态";
            require(manager.receiveEvent("chess", kToken, older).has_value() &&
                      manager.runningActivities()[0]["status"] == "等待用户落子",
              "old event overwrote new state");
            event = changeSnapshot("已暂停");
            event["mode"] = "silent";
            event["text"] = "暂停通知";
            require(manager.receiveEvent("chess", kToken, event).has_value() &&
                      manager.runningActivities().size() == 1 && events.back().mode == "silent",
              "pause removed activity");

            auto command = json{{"instruction", "恢复棋局"}, {"parameters", {{"speed", 2}}}};
            const auto commandResult =
              co_await manager.invoke("command", "chess", command, origin("private:user-b", "user-b"));
            require(commandResult.value("success", false), "cross-session command rejected");
            {
                std::lock_guard lock(fixtureMutex);
                require(lastCommand["caller"] == origin("private:user-b", "user-b") &&
                          lastCommand["origin"] == origin() && lastCommand["arguments"] == command,
                  "caller overwrote original source or parameters");
            }
            const auto details = co_await manager.invoke("details", "chess", json::object(), origin());
            require(details.contains("board"), "full details not returned");
            auto &store = Config::instance();
            require(store.initialize((root / "config.json").string()).has_value(), "fixture config failed");
            auto removed = config;
            removed["servers"] = json::array();
            require(!manager.saveConfiguration(removed, store), "running activity config removed");

            ActivityManager recovered((root / "state.json").string());
            require(recovered.initialize(config).has_value() && recovered.runningActivities().size() == 1,
              "restart discarded active activity");
            co_await recovered.synchronize("chess");
            require(recovered.configuration()["servers"][0]["reachable"] == true, "restart failed to reconcile");
            {
                std::lock_guard lock(fixtureMutex);
                unavailable = true;
            }
            co_await recovered.synchronize("chess");
            require(recovered.runningActivities().size() == 1 &&
                      getStr(recovered.runningActivities()[0], "status").find("状态未确认") != std::string::npos,
              "disconnect silently removed active activity");
            {
                std::lock_guard lock(fixtureMutex);
                unavailable = false;
            }
            co_await recovered.synchronize("chess");
            const auto stopped = co_await manager.invoke("stop", "chess", json::object(), origin());
            require(stopped.value("accepted", false) && manager.runningActivities().size() == 1,
              "stop acceptance falsely treated as finished");
            event = changeSnapshot("棋局已结束", false);
            event["text"] = "棋局结果";
            event["mode"] = "force";
            event["target_session"] = "private:user-b";
            co_await manager.synchronize("chess");
            const auto notificationsBeforeFinish = events.size();
            require(manager.receiveEvent("chess", kToken, event).has_value() && manager.runningActivities().empty(),
              "finish did not remove activity");
            require(events.size() == notificationsBeforeFinish + 1,
              "status query consumed completion notification before callback arrived");
            require(
              events.back().mode == "force" && events.back().sessionId == SessionId::privateChat(UserId{"user-b"}),
              "force/explicit destination not preserved");
            auto resurrect = event;
            resurrect["sequence"] = 1000;
            resurrect["running"] = true;
            require(!manager.receiveEvent("chess", kToken, resurrect), "ended run resurrected");
            {
                std::lock_guard lock(fixtureMutex);
                loseStartResponse = true;
            }
            const auto uncertain = co_await manager.invoke("start", "chess", json::object(), origin());
            require(uncertain.value("success", true) == false && manager.runningActivities().size() == 1,
              "lost start response failed to recover running instance");
            require(!manager.receiveEvent("chess", kToken, event), "old run completion terminated new run");
            {
                std::lock_guard lock(fixtureMutex);
                require(calls["start"] == 2, "uncertain start was retried");
                loseStartResponse = false;
            }

            ActivityManager cold((root / "cold-state.json").string());
            require(cold.initialize(config).has_value(), "cold recovery setup failed");
            co_await cold.synchronize("chess");
            require(cold.runningActivities().size() == 1, "remote running activity could not be adopted");
            const auto plugin = ActivityToolsPlugin{};
            auto &registry = ToolRegistry::instance();
            require(
              registry.registerPlugin(std::string(plugin.id()), [&](ToolRegistry &r) { plugin.registerTools(r); }),
              "activity tools registration failed");
            require(registry.hasTool("start_activity") && registry.hasTool("command_activity") &&
                      registry.hasTool("stop_activity") && registry.hasTool("get_activity_details"),
              "tools missing");
            for (const auto &tool: registry.getTools({.isPrivateSession = true, .replyOnly = true}))
                require(getStr(atOrNull(tool, "function"), "name").find("activity") == std::string::npos,
                  "activity tools bypassed final reply-only round");

            // 实际 HTTP 回调入口不使用管理员 Cookie，仍严格检查服务密钥。
            require(ActivityManager::instance().initialize(configuration(endpoint)).has_value(),
              "callback manager init failed");
            co_await ActivityManager::instance().synchronize("chess");
            auto callbackEvent = changeSnapshot("回调测试");
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setBody(callbackEvent.dump());
            auto controller = ActivityController{};
            drogon::HttpResponsePtr response;
            controller.event(request, [&](const auto &r) { response = r; });
            require(response->statusCode() == drogon::k401Unauthorized, "callback without auth accepted");
            request->addHeader("Authorization", std::string("Bearer ") + kToken);
            controller.event(request, [&](const auto &r) { response = r; });
            require(response->statusCode() == drogon::k200OK, "authenticated callback failed");
            ActivityManager::instance().stop();
            manager.stop();
            recovered.stop();
            cold.stop();
            co_await testWorkflow(endpoint, root);
            exitCode = 0;
            std::cout << "All activity protocol tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
    }
} // namespace

auto main() -> int {
    auto root = std::filesystem::temp_directory_path() / ("insoulforge-activities-" + drogon::utils::getUuid());
    std::filesystem::create_directories(root);
    // 单例持久化也隔离到临时运行目录，绝不接触项目 data。
    const auto original = std::filesystem::current_path();
    std::filesystem::current_path(root);
    drogon::app().addListener("127.0.0.1", 0).setThreadNum(1);
    drogon::app().registerHandler("/activity-model",
      [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          const auto body = json::parse(request->body());
          const auto isRouter = getStr(body, "model") == "router";
          if (isRouter)
              ++routerRequests;
          else {
              ++executorRequests;
              const auto active = ++concurrentExecutors;
              if (active > maxConcurrentExecutors)
                  maxConcurrentExecutors = active;
              std::lock_guard lock(fixtureMutex);
              lastModelContext = json::parse(getStr(body["messages"][1], "content"));
          }
          const auto response = jsonResponse({{"choices",
            json::array(
              {{{"message", {{"content", isRouter ? "{\"action\":\"skip\",\"reason\":\"fixture\"}" : "ok"}}}}})}});
          if (isRouter)
              callback(response);
          else
              drogon::app().getLoop()->runAfter(0.15, [callback = std::move(callback), response] {
                  --concurrentExecutors;
                  callback(response);
              });
      },
      {drogon::Post});
    for (const auto *path: {"/send_group_msg", "/send_private_msg"})
        drogon::app().registerHandler(path,
          [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
              callback(jsonResponse({{"status", "ok"}, {"retcode", 0}, {"data", {{"message_id", ++deliveries}}}}));
          },
          {drogon::Post});
    for (const auto *operation: {"status", "start", "command", "stop", "details"}) {
        drogon::app().registerHandler(std::string("/") + operation,
          [operation](
            const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
              auto result = json::object();
              auto response = drogon::HttpResponse::newHttpResponse();
              bool delay = false;
              {
                  std::lock_guard lock(fixtureMutex);
                  ++calls[operation];
                  if (request->getHeader("authorization") != std::string("Bearer ") + kToken) {
                      response->setStatusCode(drogon::k401Unauthorized);
                  } else if (unavailable)
                      response->setStatusCode(drogon::k503ServiceUnavailable);
                  else {
                      const auto body = json::parse(request->body());
                      if (std::string(operation) == "start") {
                          remote = {{"running", true}, {"run_id", body["run_id"]}, {"sequence", 0},
                            {"origin", body["origin"]}, {"status", "等待开始"}};
                          result = {{"snapshot", remote}, {"url", "http://" + request->getHeader("host") + "/play"}};
                          delay = delayStart;
                          startReceived = true;
                          if (loseStartResponse)
                              response->setStatusCode(drogon::k503ServiceUnavailable);
                      } else if (std::string(operation) == "status")
                          result = remote;
                      else if (std::string(operation) == "command") {
                          lastCommand = body;
                          result = {{"success", true}};
                      } else if (std::string(operation) == "stop")
                          result = {{"accepted", true}};
                      else
                          result = {{"snapshot", remote}, {"board", {{"turn", "user"}}}};
                  }
              }
              response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              response->setBody(result.dump());
              if (delay)
                  drogon::app().getLoop()->runAfter(
                    0.15, [callback = std::move(callback), response] { callback(response); });
              else
                  callback(response);
          },
          {drogon::Post});
    }
    auto client = trantor::EventLoopThread{};
    client.run();
    int exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        client.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                co_await run(endpoint, root, exitCode);
                client.getLoop()->quit();
            });
        });
    });
    drogon::app().run();
    client.wait();
    std::filesystem::current_path(original);
    std::filesystem::remove_all(root);
    return exitCode;
}
