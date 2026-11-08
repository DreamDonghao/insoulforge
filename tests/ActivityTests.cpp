/// @file ActivityTests.cpp
/// @brief 使用本地模拟服务验证全局活动协议，不启动机器人或访问外部服务。
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <drogon/WebSocketClient.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <jpeglib.h>
#include <trantor/net/EventLoopThread.h>

#include <admin/events/ActivityWebSocket.hpp>
#include <admin/http/ActivityController.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/image/Image.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/EventWorkflow.hpp>
#include <workflow/activity/ActivityManager.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/image/ImageReferences.hpp>
#include <workflow/agent/tools/plugins/ActivityToolsPlugin.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/session/SessionConfigManager.hpp>
#include <workflow/session/SessionStore.hpp>

namespace {
    using namespace insoulforge;
    constexpr auto kToken = "activity-fixture-secret";
    std::mutex fixtureMutex;
    std::atomic<int> executorRequests{0}, routerRequests{0}, deliveries{0}, concurrentExecutors{0},
      maxConcurrentExecutors{0};
    json lastModelContext;
    void require(bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }
    auto configuration() -> json { return {{"token", kToken}, {"servers", json::array()}}; }
    auto registered(const std::string &name) -> json {
        const auto config = ActivityManager::instance().configuration();
        for (const auto &service: config["servers"]) {
            if (getStr(service, "name") == name) {
                return service;
            }
        }
        return nullptr;
    }
    template<typename Predicate>
    auto waitFor(Predicate predicate, const char *message) -> drogon::Task<> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!predicate()) {
            require(std::chrono::steady_clock::now() < deadline, message);
            co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), .01);
        }
    }
    auto pngBytes() -> std::string {
        return drogon::utils::base64Decode(
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC");
    }
    auto testWorkflow(std::string endpoint, std::filesystem::path root, std::function<void(json)> notify)
      -> drogon::Task<> {
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
        auto &workflow = EventWorkflow::instance();
        require(workflow.initialize(database, config, agent).has_value(), "workflow initialization failed");
        const auto session = SessionId::group("12345");
        SessionStore::enableSession(session);
        const auto image = Image::fromBytes(pngBytes());
        require(image.has_value(), "workflow fixture image failed");
        notify({{"type", "notify"}, {"target_session", session.toString()}, {"mode", "silent"},
          {"attachments", json::array({{{"file_id", image->id()}}})}});
        co_await waitFor(
          [&] { return SessionConfigManager::getConfig(session).allMesCount == 1; }, "silent not recorded");
        require(executorRequests == 0 && routerRequests == 0, "silent event invoked model");
        notify({{"type", "notify"}, {"target_session", session.toString()}, {"mode", "silent"},
          {"attachments", json::array({{{"file_id", image->id()}}})}});
        co_await waitFor([&] { return SessionConfigManager::getConfig(session).allMesCount == 2; },
          "duplicate notification was deduplicated");
        notify({{"type", "notify"}, {"target_session", session.toString()}, {"mode", "judge"},
          {"attachments", json::array({{{"file_id", image->id()}}})}});
        co_await waitFor(
          [&] { return routerRequests == 1 && SessionConfigManager::getConfig(session).allMesCount == 3; },
          "judge not routed");
        require(executorRequests == 0, "judge ignored skip decision");
        notify({{"type", "notify"}, {"target_session", session.toString()}, {"mode", "force"},
          {"attachments", json::array({{{"file_id", image->id()}}})}});
        co_await waitFor([&] { return executorRequests == 1; }, "force not delivered to executor");
        workflow.enqueueActivityMessage(session, "继续", "chess", "force");
        co_await waitFor([&] { return deliveries == 2; }, "queued force events lost");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(routerRequests == 1 && maxConcurrentExecutors == 1, "force routed or replies ran concurrently");
        {
            std::lock_guard lock(fixtureMutex);
            bool foundImage = false;
            for (const auto &message: lastModelContext["recent_messages"]) {
                for (const auto &segment: atOrNull(message, "segments")) {
                    foundImage = foundImage || getStr(segment, "image_id") == image->id();
                }
            }
            require(foundImage, "image-only activity asset did not reach model");
            const auto references = ImageReferences::resolve(json::array({image->id()}),
              ToolCallContext{.sessionId = session, .messageSnapshot = lastModelContext["recent_messages"]});
            require(references && references->size() == 1 && references->front() == image->id(),
              "image tool cannot resolve activity asset");
            require(lastModelContext.contains("activities"), "model context omitted activities");
            auto keys = std::vector<std::string>{};
            for (const auto &[key, value]: lastModelContext.items()) {
                keys.push_back(key);
            }
            require(keys.size() >= 2 && keys[keys.size() - 2] == "activities" && keys.back() == "execution_state",
              "activities precede the per-request execution state");
            for (const auto &message: lastModelContext["recent_messages"]) {
                require(!atOrNull(message, "activity").contains("origin"), "notification has obsolete origin");
            }
        }
        const auto privateSession = SessionId::privateChat(UserId{"67890"});
        SessionStore::enableSession(privateSession);
        workflow.enqueueActivityMessage(privateSession, "私聊", "chess", "force");
        co_await waitFor([&] { return deliveries == 3; }, "private event not delivered");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        {
            std::lock_guard lock(fixtureMutex);
            require(
              !lastModelContext.contains("response_requirements"), "activity added reply strategy to private chat");
        }
        agent.setRunning(false);
        workflow.enqueueActivityMessage(session, "禁用", "chess", "force");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(deliveries == 3, "force bypassed bot run switch");
        agent.setRunning(true);
        SessionStore::disableSession(session);
        workflow.enqueueActivityMessage(session, "禁用", "chess", "force");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(deliveries == 3, "force bypassed session switch");
        workflow.flushMessageListsToStorage();
        database.close();
    }

    struct MockService {
        drogon::WebSocketClientPtr client;
        std::atomic<bool> ready{false}, attempted{false}, accepted{false}, closed{false}, hold{false};
        std::atomic<int> calls{0}, concurrent{0}, maxConcurrent{0};
        json lastRequest;
        void close() {
            const auto source = client;
            source->getLoop()->runInLoop([source] { source->stop(); });
        }
        void send(json message) { client->getConnection()->send(dumpJson(message)); }
    };
    auto connectService(const std::string &endpoint, const std::string &name = "service",
      const std::string &token = kToken, bool valid = true) -> std::shared_ptr<MockService> {
        auto service = std::make_shared<MockService>();
        auto wsEndpoint = endpoint;
        wsEndpoint.replace(0, 4, "ws");
        service->client = drogon::WebSocketClient::newWebSocketClient(wsEndpoint);
        const auto weak = std::weak_ptr(service);
        service->client->setConnectionClosedHandler([weak](const auto &source) {
            if (auto value = weak.lock()) {
                value->closed = true;
            }
            source->stop();
        });
        service->client->setMessageHandler([weak](std::string &&text, const auto &source, auto type) {
            if (type != drogon::WebSocketMessageType::Text) {
                return;
            }
            auto service = weak.lock();
            if (!service) {
                return;
            }
            const auto message = json::parse(text);
            if (getStr(message, "type") == "ready") {
                service->ready = true;
                return;
            }
            if (getStr(message, "type") != "request") {
                return;
            }
            {
                std::lock_guard lock(fixtureMutex);
                service->lastRequest = message;
            }
            ++service->calls;
            const auto current = ++service->concurrent;
            if (current > service->maxConcurrent) {
                service->maxConcurrent = current;
            }
            if (service->hold) {
                return;
            }
            const auto parameters = message["parameters"];
            auto response = json{{"type", "response"}, {"success", true}, {"result", parameters}};
            if (parameters.contains("file")) {
                response["attachments"] = json::array({{{"file_id", parameters["file"]}}});
            }
            if (parameters.contains("fail")) {
                response = {{"type", "response"}, {"success", false}, {"error", "rejected"}};
            }
            source->getLoop()->runAfter(.1, [weak, source, response] {
                if (auto value = weak.lock()) {
                    --value->concurrent;
                    source->getConnection()->send(dumpJson(response));
                }
            });
        });
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/activities/ws");
        request->addHeader("Authorization", "Bearer " + token);
        service->client->connectToServer(request, [weak, name, valid](auto result, const auto &, const auto &source) {
            if (auto value = weak.lock()) {
                value->accepted = result == drogon::ReqResult::Ok;
                if (value->accepted) {
                    auto message = json{{"type", "register"}, {"name", name}, {"description", "service description"},
                      {"instructions", "custom parameters"}, {"active", false}, {"status", ""}};
                    if (!valid) {
                        message.erase("active");
                    }
                    source->getConnection()->send(dumpJson(message));
                }
                value->attempted = true;
            }
        });
        return service;
    }
    auto run(const std::string endpoint, const std::filesystem::path root, int &exitCode) -> drogon::Task<> {
        std::vector<std::shared_ptr<MockService>> clients;
        auto &manager = ActivityManager::instance();
        try {
            auto &store = Config::instance();
            require(store.initialize((root / "config.json").string()).has_value(), "config initialization failed");
            require(manager.initialize(configuration()).has_value(), "manager initialization failed");
            manager.start(*drogon::app().getLoop());
            auto connect = [&](const std::string &name, const std::string &token = kToken, bool valid = true) {
                auto client = connectService(endpoint, name, token, valid);
                clients.push_back(client);
                return client;
            };
            auto bad = connect("bad", "wrong");
            co_await waitFor([&] { return bad->attempted.load(); }, "authentication did not finish");
            require(!bad->accepted, "bad token accepted");
            auto malformed = connect("malformed", kToken, false);
            co_await waitFor([&] { return malformed->closed.load(); }, "old registration accepted");
            auto first = connect("service");
            co_await waitFor([&] { return first->ready.load(); }, "registration not ready");
            require(first->calls == 0 && manager.directory().size() == 1 && manager.activeActivities().empty(),
              "registration queried status or idle service hidden");
            require(manager.directory()[0]["instructions"] == "custom parameters", "instructions omitted");
            first->send({{"type", "status"}, {"active", true}, {"status", "current"}});
            co_await waitFor([&] { return manager.activeActivities().size() == 1; }, "state not cached");
            first->send({{"type", "status"}, {"active", false}, {"status", ""}});
            co_await waitFor([&] { return manager.activeActivities().empty(); }, "inactive state retained");
            auto result = co_await manager.invoke("service", {{"custom", "value"}});
            require(getBool(result, "success") && result["result"]["custom"] == "value", "parameters changed");
            {
                std::lock_guard lock(fixtureMutex);
                require(first->lastRequest.size() == 2 && first->lastRequest.contains("parameters"),
                  "business or request identifiers injected");
            }
            result = co_await manager.invoke("service", {{"fail", true}});
            require(!getBool(result, "success") && result["error"] == "rejected", "business failure lost");
            // Queue two calls on one connection and run another connection concurrently.
            auto other = connect("other");
            co_await waitFor([&] { return other->ready.load(); }, "second service missing");
            auto completed = std::make_shared<std::atomic<int>>(0);
            auto results = std::make_shared<std::vector<json>>(3);
            for (int index = 0; index < 3; ++index) {
                drogon::async_run([&, index, completed, results]() -> drogon::Task<> {
                    (*results)[index] = co_await manager.invoke(index == 2 ? "other" : "service", {{"index", index}});
                    ++*completed;
                });
            }
            co_await waitFor(
              [&] { return first->concurrent == 1 && other->concurrent == 1; }, "connections not parallel");
            co_await waitFor([&] { return completed->load() == 3; }, "queued requests did not finish");
            require(first->maxConcurrent == 1 && (*results)[0]["result"]["index"] == 0 &&
                      (*results)[1]["result"]["index"] == 1,
              "connection requests interleaved");
            auto http = drogon::HttpClient::newHttpClient(endpoint);
            auto uploadRequest = drogon::HttpRequest::newHttpRequest();
            uploadRequest->setPath("/activities/files");
            uploadRequest->setMethod(drogon::Post);
            uploadRequest->addHeader("Authorization", std::string("Bearer ") + kToken);
            uploadRequest->setContentTypeString("multipart/form-data; boundary=fixture-boundary");
            uploadRequest->setBody(
              "--fixture-boundary\r\nContent-Disposition: form-data; "
              "name=\"name\"\r\n\r\nservice\r\n--fixture-boundary\r\nContent-Disposition: form-data; name=\"file\"; "
              "filename=\"image.png\"\r\nContent-Type: image/png\r\n\r\n" +
              pngBytes() + "\r\n--fixture-boundary--\r\n");
            const auto uploaded = co_await http->sendRequestCoro(uploadRequest);
            require(uploaded->statusCode() == drogon::k200OK && getBool(json::parse(uploaded->body()), "success"),
              "multipart upload contract failed");
            auto upload = manager.uploadFile("service", pngBytes());
            require(upload.has_value(), "valid PNG rejected");
            require(!manager.uploadFile("service", "not an image") && !manager.uploadFile("missing", pngBytes()),
              "invalid upload accepted");
            ActivityToolsPlugin plugin;
            auto &registry = ToolRegistry::instance();
            require(
              registry.registerPlugin("builtin.activity", [&](ToolRegistry &target) { plugin.registerTools(target); }),
              "plugin registration failed");
            ToolCallContext ctx;
            ctx.toolImages = std::make_shared<std::vector<Image>>();
            const auto output = co_await registry.executeTool(
              "request_activity", {{"name", "service"}, {"parameters", {{"file", (*upload)["file_id"]}}}}, ctx);
            auto response = json::parse(output);
            require(response["attachments"][0].contains("image_id") && ctx.toolImages->size() == 1,
              "response attachment unusable");
            const auto resolved = ImageReferences::resolve(json::array({response["attachments"][0]["image_id"]}), ctx);
            require(resolved.has_value() && resolved->size() == 1, "response image not visible to subsequent tools");
            // A service cannot reference another service's upload.
            result = co_await manager.invoke("other", {{"file", (*upload)["file_id"]}});
            require(!getBool(result, "success"), "cross-service upload accepted");
            co_await waitFor([&] { return other->closed.load(); }, "invalid response did not close connection");
            require(manager.setEnabled("service", false, store).has_value(), "disable failed");
            require(manager.directory().empty() && manager.activeActivities().empty(), "disabled service exposed");
            require(
              !getBool(co_await manager.invoke("service", json::object()), "success"), "disabled service invoked");
            require(manager.setEnabled("service", true, store).has_value(), "enable failed");
            // Replacement disconnects the old connection and settles pending calls.
            first->hold = true;
            completed->store(0);
            drogon::async_run([&, completed, results]() -> drogon::Task<> {
                (*results)[0] = co_await manager.invoke("service", json::object());
                ++*completed;
            });
            co_await waitFor([&] { return first->concurrent == 1; }, "pending request not sent");
            first->send({{"type", "status"}, {"active", true}, {"status", "updated while waiting"}});
            co_await waitFor(
              [&] { return manager.activeActivities().size() == 1; }, "state blocked by pending request");
            auto replacement = connect("service");
            co_await waitFor(
              [&] { return replacement->ready.load() && first->closed.load() && completed->load() == 1; },
              "takeover did not cancel old request");
            require(!getBool((*results)[0], "success") && manager.activeActivities().empty(),
              "takeover reused old state or result");
            replacement->hold = true;
            completed->store(0);
            for (int index = 0; index < 2; ++index) {
                drogon::async_run([&, index, completed, results]() -> drogon::Task<> {
                    (*results)[index] = co_await manager.invoke("service", {{"index", index}});
                    ++*completed;
                });
            }
            co_await waitFor([&] { return completed->load() == 2; }, "timeout did not complete queue");
            co_await waitFor([&] { return replacement->closed.load(); }, "timeout did not close connection");
            require(replacement->calls == 1 && !getBool((*results)[0], "success") &&
                      getStr((*results)[1], "error").find("未执行") != std::string::npos,
              "timeout retried or sent queued request");
            auto live = connect("service");
            co_await waitFor([&] { return live->ready.load(); }, "reconnect failed");
            live->send({{"type", "status"}, {"active", true}, {"status", "current"}});
            co_await waitFor([&] { return manager.activeActivities().size() == 1; }, "reconnect state missing");
            const auto image = manager.uploadFile("service", pngBytes());
            require(image.has_value(), "workflow image upload failed");
            co_await testWorkflow(endpoint, root, [live](json message) { live->send(std::move(message)); });
            live->send({{"type", "notify"}, {"text", "no target"}});
            co_await waitFor([&] { return live->closed.load(); }, "notification target not required");
            require(manager.activeActivities().empty(), "offline state retained in context");
            auto removed = connect("remove");
            co_await waitFor([&] { return removed->ready.load(); }, "remove fixture failed");
            removed->send({{"type", "status"}, {"active", true}, {"status", "working"}});
            require(manager.removeService("remove", store).has_value(), "online delete failed");
            co_await waitFor([&] { return removed->closed.load(); }, "delete did not disconnect");
            require(registered("remove").is_null(), "deleted service retained");
            require(!std::filesystem::exists(root / "data/activity-state.json"), "business state persisted");
            exitCode = 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            exitCode = 1;
        }
        manager.stop();
        for (const auto &client: clients) {
            client->close();
        }
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), .1);
        drogon::app().quit();
    }
} // namespace
auto main() -> int {
    auto root = std::filesystem::temp_directory_path() / ("insoulforge-activities-" + drogon::utils::getUuid());
    std::filesystem::create_directories(root);
    // 测试运行目录隔离，绝不接触项目 data。
    const auto original = std::filesystem::current_path();
    std::filesystem::current_path(root);
    drogon::app()
      .addListener("127.0.0.1", 0)
      .setThreadNum(1)
      .setClientMaxWebSocketMessageSize(1024 * 1024)
      .setClientMaxBodySize(25U * 1024U * 1024U)
      .setClientMaxMemoryBodySize(25U * 1024U * 1024U);
    drogon::app().registerHandler("/activity-model",
      [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          const auto body = json::parse(request->body());
          const auto isRouter = getStr(body, "model") == "router";
          if (isRouter) {
              ++routerRequests;
          } else {
              ++executorRequests;
              const auto active = ++concurrentExecutors;
              if (active > maxConcurrentExecutors) {
                  maxConcurrentExecutors = active;
              }
              std::lock_guard lock(fixtureMutex);
              for (const auto &message: body["messages"]) {
                  if (getStr(message, "role") == "user") {
                      lastModelContext = json::parse(getStr(message, "content"));
                  }
              }
          }
          const auto response = jsonResponse({{"choices",
            json::array(
              {{{"message", {{"content", isRouter ? "{\"action\":\"skip\",\"reason\":\"fixture\"}" : "ok"}}}}})}});
          if (isRouter) {
              callback(response);
          } else {
              drogon::app().getLoop()->runAfter(0.15, [callback = std::move(callback), response] {
                  --concurrentExecutors;
                  callback(response);
              });
          }
      },
      {drogon::Post});
    for (const auto *path: {"/send_group_msg", "/send_private_msg"}) {
        drogon::app().registerHandler(path,
          [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
              callback(jsonResponse({{"status", "ok"}, {"retcode", 0}, {"data", {{"message_id", ++deliveries}}}}));
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
