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
#include <workflow/OneBotEventWorkflow.hpp>
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
    auto fixtureMutex = std::mutex{};
    auto remote = json{{"running", false}};
    auto calls = std::map<std::string, int>{};
    auto lastCommand = json{};
    std::atomic<bool> unavailable{false};
    std::atomic<bool> loseStartResponse{false};
    std::atomic<bool> delayStart{false};
    std::atomic<bool> delayStatus{false};
    std::atomic<bool> largeDetails{false};
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
    auto configuration() -> json { return {{"token", kToken}, {"servers", json::array()}}; }
    auto callCount(const std::string &operation) -> int {
        std::lock_guard lock(fixtureMutex);
        return calls[operation];
    }
    auto registered(const std::string &name) -> json {
        const auto config = ActivityManager::instance().configuration();
        for (const auto &service: config["servers"])
            if (getStr(service, "name") == name)
                return service;
        return nullptr;
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
    auto pngBytes() -> std::string {
        return drogon::utils::base64Decode(
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC");
    }
    auto jpegBytes() -> std::string {
        auto image = jpeg_compress_struct{};
        auto error = jpeg_error_mgr{};
        image.err = jpeg_std_error(&error);
        jpeg_create_compress(&image);
        unsigned char *output = nullptr;
        unsigned long size = 0;
        jpeg_mem_dest(&image, &output, &size);
        image.image_width = 1;
        image.image_height = 1;
        image.input_components = 3;
        image.in_color_space = JCS_RGB;
        jpeg_set_defaults(&image);
        jpeg_start_compress(&image, TRUE);
        unsigned char pixel[] = {255, 0, 0};
        auto *row = pixel;
        jpeg_write_scanlines(&image, &row, 1);
        jpeg_finish_compress(&image);
        const auto bytes = std::string(reinterpret_cast<char *>(output), size);
        std::free(output);
        jpeg_destroy_compress(&image);
        return bytes;
    }
    auto uploadRequest(std::string name, std::string runId, std::string bytes, std::string token = kToken)
      -> drogon::HttpRequestPtr {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/activities/files");
        request->setMethod(drogon::Post);
        if (!token.empty())
            request->addHeader("Authorization", "Bearer " + token);
        request->setContentTypeString("multipart/form-data; boundary=fixture-boundary");
        request->setBody(
          "--fixture-boundary\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\n" + name +
          "\r\n--fixture-boundary\r\nContent-Disposition: form-data; name=\"run_id\"\r\n\r\n" + runId +
          "\r\n--fixture-boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"wrong.gif\"\r\n"
          "Content-Type: image/gif\r\n\r\n" +
          bytes + "\r\n--fixture-boundary--\r\n");
        return request;
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
        const auto image = Image::fromBytes(pngBytes());
        require(image.has_value(), "workflow fixture image failed");
        workflow.enqueueActivityMessage(session, "", "chess", "silent", origin(), {*image});
        co_await waitFor(
          [&] { return SessionConfigManager::getConfig(session).allMesCount == 1; }, "silent not recorded");
        require(executorRequests == 0 && routerRequests == 0, "silent event invoked model");
        workflow.enqueueActivityMessage(session, "", "chess", "judge", origin(), {*image});
        co_await waitFor(
          [&] { return routerRequests == 1 && SessionConfigManager::getConfig(session).allMesCount == 2; },
          "judge not routed");
        require(executorRequests == 0, "judge ignored skip decision");
        workflow.enqueueActivityMessage(session, "", "chess", "force", origin(), {*image});
        co_await waitFor([&] { return executorRequests == 1; }, "force not delivered to executor");
        workflow.enqueueActivityMessage(session, "继续", "chess", "force", origin());
        co_await waitFor([&] { return deliveries == 2; }, "queued force events lost");
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 0.05);
        require(routerRequests == 1 && maxConcurrentExecutors == 1, "force routed or replies ran concurrently");
        {
            std::lock_guard lock(fixtureMutex);
            bool foundImage = false;
            for (const auto &message: lastModelContext["recent_messages"])
                for (const auto &segment: atOrNull(message, "segments"))
                    foundImage = foundImage || getStr(segment, "image_id") == image->id();
            require(foundImage, "image-only activity asset did not reach model");
            const auto references = ImageReferences::resolve(json::array({image->id()}),
              ToolCallContext{.sessionId = session, .messageSnapshot = lastModelContext["recent_messages"]});
            require(references && references->size() == 1 && references->front() == image->id(),
              "image tool cannot resolve activity asset");
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
    struct MockService {
        drogon::WebSocketClientPtr client;
        std::atomic<bool> ready{false};
        std::atomic<bool> attempted{false};
        std::atomic<bool> accepted{false};
        std::atomic<bool> closed{false};
        std::atomic<int> acks{0};
        json lastAck;
        void close() {
            auto source = client;
            source->getLoop()->runInLoop([source] { source->stop(); });
        }
        void event(const json &value) {
            client->getConnection()->send(dumpJson(json{{"type", "event"}, {"event", value}}));
        }
    };
    auto connectService(const std::string &endpoint, const std::string &name = "chess",
      const std::string &token = kToken, const std::string &description = "一起下棋", bool autoRegister = true)
      -> std::shared_ptr<MockService> {
        auto service = std::make_shared<MockService>();
        auto wsEndpoint = endpoint;
        wsEndpoint.replace(0, 4, "ws");
        service->client = drogon::WebSocketClient::newWebSocketClient(wsEndpoint);
        const auto weak = std::weak_ptr(service);
        service->client->setConnectionClosedHandler([weak](const auto &source) {
            if (auto service = weak.lock())
                service->closed = true;
            source->stop();
        });
        service->client->setMessageHandler([weak](std::string &&text, const auto &source, auto type) {
            if (type != drogon::WebSocketMessageType::Text)
                return;
            auto service = weak.lock();
            if (!service)
                return;
            const auto message = json::parse(text);
            if (getStr(message, "type") == "ready") {
                service->ready = true;
                return;
            }
            if (getStr(message, "type") == "event_ack") {
                std::lock_guard lock(fixtureMutex);
                service->lastAck = message;
                ++service->acks;
                return;
            }
            const auto operation = getStr(message, "operation");
            const auto body = message["body"];
            json result;
            bool delayed = false;
            bool lost = false;
            {
                std::lock_guard lock(fixtureMutex);
                ++calls[operation];
                if (operation == "status") {
                    result = remote;
                    delayed = delayStatus;
                } else if (operation == "start") {
                    remote = {{"running", true}, {"run_id", body["run_id"]}, {"sequence", 0}, {"status", "等待开始"},
                      {"origin", body["origin"]}};
                    result = {{"snapshot", remote}, {"url", "http://fixture/play"}};
                    startReceived = true;
                    delayed = delayStart;
                    lost = loseStartResponse;
                } else if (operation == "command") {
                    lastCommand = body;
                    if (unavailable)
                        return; // 测试真正的请求超时，不执行操作，也不发响应。
                    result = {{"success", true}};
                } else if (operation == "stop")
                    result = {{"accepted", true}};
                else {
                    result = {{"snapshot", remote}, {"board", {{"turn", "user"}}}};
                    if (largeDetails)
                        result["extra"] = std::string(150 * 1024, 'x');
                }
            }
            if (lost) {
                source->stop(); // 已启动，但响应丢失。
                return;
            }
            const auto response = dumpJson(
              json{{"type", "response"}, {"request_id", message["request_id"]}, {"success", true}, {"result", result}});
            if (delayed)
                source->getLoop()->runAfter(.15, [source, response] { source->getConnection()->send(response); });
            else
                source->getConnection()->send(response);
        });
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPathEncode(false);
        request->setPath("/activities/ws");
        request->addHeader("Authorization", "Bearer " + token);
        service->client->connectToServer(
          request, [weak, name, description, autoRegister](auto result, const auto &, const auto &source) {
              if (auto service = weak.lock()) {
                  service->accepted = result == drogon::ReqResult::Ok;
                  if (service->accepted && autoRegister)
                      source->getConnection()->send(
                        dumpJson(json{{"type", "register"}, {"name", name}, {"description", description}}));
                  service->attempted = true;
              }
          });
        return service;
    }
    auto run(const std::string endpoint, const std::filesystem::path root, int &exitCode) -> drogon::Task<> {
        std::vector<std::shared_ptr<MockService>> clients;
        auto &manager = ActivityManager::instance();
        try {
            auto &store = Config::instance();
            require(store.initialize((root / "config.json").string()).has_value(), "fixture config failed");
            auto config = configuration();
            ActivityManager legacy((root / "legacy-state.json").string());
            require(
              legacy
                  .initialize({{"servers",
                    json::array({{{"name", "old"}, {"description", "旧描述"}, {"enabled", true}, {"token", kToken}}})}})
                  .has_value() &&
                !legacy.authorized(kToken),
              "legacy key silently became global token");
            require(!legacy.configuration()["servers"][0].contains("token"), "legacy per-service token retained");
            require(manager.initialize(config).has_value(), "manager init failed");
            manager.start(*drogon::app().getLoop());
            require(!manager.authorized("wrong") && !manager.authorized("") && manager.authorized(kToken),
              "invalid auth accepted");
            auto invalid = config;
            invalid["servers"] = json::array({{{"name", "duplicate"}, {"description", ""}, {"enabled", true}},
              {{"name", "duplicate"}, {"description", ""}, {"enabled", true}}});
            require(!manager.initialize(invalid), "duplicate config accepted");
            require(manager.directory().dump().find(kToken) == std::string::npos, "secret leaked to model");
            require(!manager.configuration().contains("callbackUrl") && manager.configuration()["servers"].empty(),
              "HTTP config retained");
            auto result = co_await manager.invoke("start", "chess", json::object(), origin());
            require(!getBool(result, "success", true), "disconnected start accepted");
            auto rejected = connectService(endpoint, "chess", "wrong");
            clients.push_back(rejected);
            co_await waitFor([&] { return rejected->attempted.load(); }, "auth handshake did not finish");
            require(!rejected->accepted, "unauthorized upgrade accepted");
            auto authRequest = drogon::HttpRequest::newHttpRequest();
            authRequest->setParameter("name", "chess");
            drogon::HttpResponsePtr denied;
            ActivityAuthentication{}.doFilter(authRequest, [&](const auto &response) { denied = response; }, [] {});
            require(denied && denied->statusCode() == drogon::k401Unauthorized, "auth filter did not return 401");
            require(!manager.saveToken("short", store), "short access token accepted");
            auto malformed = connectService(endpoint, "");
            clients.push_back(malformed);
            co_await waitFor([&] { return malformed->closed.load(); }, "invalid registration not closed");
            auto unregistered = connectService(endpoint, "unused", kToken, "", false);
            clients.push_back(unregistered);
            // 特殊字符名称的实际管理路由，以及禁用后重连的自动注册行为。
            const auto alias = std::string("井字棋 /测试");
            auto extra = connectService(endpoint, alias);
            clients.push_back(extra);
            co_await waitFor([&] { return extra->ready.load(); }, "automatic registration failed");
            require(getBool(registered(alias), "enabled"), "new service not enabled automatically");
            auto patchRequest = drogon::HttpRequest::newHttpRequest();
            patchRequest->setMethod(drogon::Patch);
            patchRequest->setPath("/admin/api/activities");
            patchRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            patchRequest->setBody(dumpJson(json{{"name", alias}, {"enabled", false}}));
            const auto httpClient = drogon::HttpClient::newHttpClient(endpoint);
            const auto patch = co_await httpClient->sendRequestCoro(patchRequest, 3);
            require(patch && patch->statusCode() == drogon::k200OK && !getBool(registered(alias), "enabled"),
              "single-service PATCH route failed");
            extra->close();
            co_await waitFor([&] { return !getBool(registered(alias), "connected"); }, "alias disconnect missing");
            {
                std::lock_guard lock(fixtureMutex);
                remote = {{"run_id", "disabled-run"}, {"sequence", 0}, {"status", "禁用期间状态"}, {"running", true},
                  {"origin", origin()}};
            }
            extra = connectService(endpoint, alias, kToken, "服务的新描述");
            clients.push_back(extra);
            co_await waitFor([&] { return extra->ready.load(); }, "disabled service cannot register");
            require(
              !getBool(registered(alias), "enabled") && getStr(registered(alias), "description") == "服务的新描述",
              "reconnect overwrote enabled flag or lost description");
            ActivityManager disabledRestore((root / "disabled-restore.json").string());
            require(disabledRestore.initialize(store.getActivitiesConfig()).has_value() &&
                      disabledRestore.directory().empty(),
              "disabled registry not persisted");
            auto staleRequest = drogon::HttpRequest::newHttpRequest();
            staleRequest->setBody(dumpJson(json{{"token", kToken}, {"servers", json::array()}}));
            drogon::HttpResponsePtr rejectedSave;
            ActivityController{}.saveConfig(staleRequest, [&](const auto &response) { rejectedSave = response; });
            require(rejectedSave->statusCode() == drogon::k400BadRequest && !registered(alias).is_null(),
              "stale full-list save overwrote registry");
            auto disabledEvent = json{{"run_id", "disabled-run"}, {"sequence", 1}, {"status", "禁用期间状态"},
              {"running", true}, {"origin", origin()}, {"mode", "force"}, {"text", "禁用通知"}};
            extra->event(disabledEvent);
            co_await waitFor([&] { return extra->acks == 1; }, "disabled state not acknowledged");
            require(
              getBool(extra->lastAck, "success") && manager.runningActivities().empty() && manager.directory().empty(),
              "disabled service exposed to model");
            disabledEvent["running"] = false;
            disabledEvent["sequence"] = 2;
            extra->event(disabledEvent);
            co_await waitFor([&] { return extra->acks == 2; }, "disabled terminal state missing");
            extra->close();
            co_await waitFor([&] { return !getBool(registered(alias), "connected"); }, "alias close missing");
            require(manager.removeService(alias, store).has_value(), "offline record cleanup failed");
            {
                std::lock_guard lock(fixtureMutex);
                remote = {{"running", false}};
            }
            extra = connectService(endpoint, alias);
            clients.push_back(extra);
            require(manager.saveToken(kToken, store).has_value(), "token save failed");
            co_await waitFor([&] { return extra->ready.load(); }, "removed service not rediscovered");
            require(getBool(registered(alias), "enabled"), "rediscovered service not enabled");
            require(store.getActivitiesConfig()["servers"].size() == 1, "token save lost auto registration");
            extra->close();
            co_await waitFor([&] { return !getBool(registered(alias), "connected"); }, "alias final close missing");
            require(manager.removeService(alias, store).has_value(), "alias cleanup failed");
            auto service = connectService(endpoint);
            clients.push_back(service);
            co_await waitFor([&] { return service->ready.load(); }, "initial sync failed");
            require(getBool(registered("chess"), "enabled") && !registered("chess").contains("token"),
              "service not automatically registered");
            require(manager.runningActivities().empty(), "idle service in context");
            require(!(co_await manager.refresh("missing")).has_value(), "unknown service synchronization accepted");
            delayStart = true;
            std::atomic<bool> done{false};
            json first;
            drogon::async_run([&]() -> drogon::Task<> {
                first = co_await manager.invoke("start", "chess", json::object(), origin());
                done = true;
            });
            co_await waitFor([&] { return startReceived.load(); }, "start missing");
            result = co_await manager.invoke("start", "chess", json::object(), origin("private:someone"));
            require(!getBool(result, "success", true), "concurrent duplicate start accepted");
            co_await waitFor([&] { return done.load(); }, "start response missing");
            require(
              first["url"] == "http://fixture/play" && manager.runningActivities().size() == 1, "start not reflected");
            require(!manager.removeService("chess", store), "running service removed");
            require(!manager.setEnabled("chess", false, store), "running service disabled");
            ActivityManager restored("data/activity-state.json");
            require(
              restored.initialize(manager.configuration()).has_value() && restored.runningActivities().size() == 1,
              "restart lost persistent run");
            changeSnapshot("漏推送，主动核对");
            auto syncRequest = drogon::HttpRequest::newHttpRequest();
            syncRequest->setMethod(drogon::Post);
            syncRequest->setPath("/admin/api/activities/sync");
            syncRequest->setBody("{\"name\":\"chess\"}");
            syncRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            const auto synced = co_await httpClient->sendRequestCoro(syncRequest, 3);
            require(
              synced->statusCode() == drogon::k200OK && getStr(registered("chess"), "status") == "漏推送，主动核对",
              "manual synchronization did not query actual state");
            delayStart = false;
            result = co_await manager.invoke("command", "chess",
              {{"instruction", "暂停"}, {"parameters", {{"cell", 5}}}}, origin("private:caller", "actual-user"));
            require(getBool(result, "success") && lastCommand["caller"] == origin("private:caller", "actual-user") &&
                      lastCommand["origin"] == origin(),
              "caller/origin lost");
            require(
              !lastCommand.contains("callback_url") && !lastCommand.contains("name"), "old transport fields leaked");
            largeDetails = true;
            result = co_await manager.invoke("details", "chess", json::object(), origin());
            largeDetails = false;
            require(
              result.contains("board") && getStr(result, "extra").size() == 150 * 1024, "large details response lost");
            delayStatus = true;
            const auto beforeStatus = callCount("status");
            done = false;
            drogon::async_run([&]() -> drogon::Task<> {
                result = co_await manager.invoke("details", "chess", json::object(), origin());
                done = true;
            });
            co_await waitFor([&] { return callCount("status") > beforeStatus; }, "post-operation status not requested");
            const auto busy = co_await manager.invoke("command", "chess", json::object(), origin());
            require(!getBool(busy, "success", true), "operation overlapped status confirmation");
            require(!(co_await manager.refresh("chess")).has_value(), "manual synchronization overlapped operation");
            co_await waitFor([&] { return done.load(); }, "delayed confirmation did not finish");
            delayStatus = false;
            const auto runId = getStr(first["snapshot"], "run_id");
            auto response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, pngBytes(), ""), 3);
            require(response->statusCode() == drogon::k401Unauthorized, "anonymous upload accepted");
            response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, pngBytes(), "wrong"), 3);
            require(response->statusCode() == drogon::k401Unauthorized, "wrong upload token accepted");
            response = co_await httpClient->sendRequestCoro(uploadRequest("chess", "stale-run", pngBytes()), 3);
            require(response->statusCode() == drogon::k400BadRequest, "stale upload run accepted");
            response = co_await httpClient->sendRequestCoro(uploadRequest("missing", runId, pngBytes()), 3);
            require(response->statusCode() == drogon::k400BadRequest, "unregistered upload accepted");
            for (const auto &bytes: {std::string("not an image"), std::string("\xff\xd8\xff\xd9", 4),
                   drogon::utils::base64Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7")}) {
                response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, bytes), 3);
                require(response->statusCode() == drogon::k400BadRequest, "unsupported or corrupt image accepted");
            }
            response = co_await httpClient->sendRequestCoro(
              uploadRequest("chess", runId, std::string(24U * 1024U * 1024U + 1, 'x')), 3);
            require(response->statusCode() == drogon::k413RequestEntityTooLarge, "oversize image accepted");
            auto malformedUpload = drogon::HttpRequest::newHttpRequest();
            malformedUpload->setPath("/activities/files");
            malformedUpload->setMethod(drogon::Post);
            malformedUpload->addHeader("Authorization", std::string("Bearer ") + kToken);
            malformedUpload->setBody("{}");
            response = co_await httpClient->sendRequestCoro(malformedUpload, 3);
            require(response->statusCode() == drogon::k400BadRequest, "non-multipart upload accepted");
            response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, jpegBytes()), 3);
            require(response->statusCode() == drogon::k200OK &&
                      getStr(json::parse(response->body()), "media_type") == "image/jpeg",
              "JPEG upload failed");
            auto corruptPng = pngBytes();
            corruptPng[45] ^= 1;
            response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, corruptPng), 3);
            require(response->statusCode() == drogon::k400BadRequest, "corrupt PNG accepted");
            auto other = connectService(endpoint, "other");
            clients.push_back(other);
            co_await waitFor([&] { return other->ready.load(); }, "other upload service not ready");
            response = co_await httpClient->sendRequestCoro(uploadRequest("other", runId, pngBytes()), 3);
            require(response->statusCode() == drogon::k200OK, "PNG multipart upload failed");
            const auto uploaded = json::parse(response->body());
            const auto fileId = getStr(uploaded, "file_id");
            require(fileId.starts_with("img_") && fileId.size() == 12 && getStr(uploaded, "media_type") == "image/png",
              "upload returned wrong ID or trusted declared MIME");
            auto imageEvent = changeSnapshot("图片事件");
            imageEvent["mode"] = "silent";
            imageEvent["attachments"] = json::array({{{"file_id", fileId}}});
            service->event(imageEvent);
            co_await waitFor([&] { return service->acks == 1; }, "cross-service attachment ack missing");
            require(!getBool(service->lastAck, "success"), "cross-service attachment accepted");
            require(getStr(registered("chess"), "status") != getStr(imageEvent, "status"),
              "rejected attachment advanced state");
            response = co_await httpClient->sendRequestCoro(uploadRequest("chess", runId, pngBytes()), 3);
            require(response->statusCode() == drogon::k200OK, "own upload failed");
            auto invalidAttachment = imageEvent;
            invalidAttachment["attachments"] = json::array({{{"file_id", "img_deadbeef"}}});
            service->event(invalidAttachment);
            co_await waitFor([&] { return service->acks == 2; }, "invalid attachment ack missing");
            require(!getBool(service->lastAck, "success"), "unknown attachment accepted");
            service->event(imageEvent);
            co_await waitFor([&] { return service->acks == 3; }, "image-only event ack missing");
            require(getBool(service->lastAck, "success"), "corrected image event rejected");
            service->event(imageEvent);
            co_await waitFor([&] { return service->acks == 4; }, "repeat image event ack missing");
            require(getBool(service->lastAck, "success"), "repeat image event rejected");
            require(
              executorRequests == 0 && routerRequests == 0 && deliveries == 0, "upload invoked model or sent image");
            other->close();
            co_await waitFor([&] { return !getBool(registered("other"), "connected"); }, "other close missing");
            require(manager.removeService("other", store, true).has_value(), "other cleanup failed");
            service->acks = 0;
            auto event = changeSnapshot("等待落子");
            event["mode"] = "silent";
            service->event(event);
            co_await waitFor([&] { return service->acks == 1; }, "event ack missing");
            service->event(event);
            co_await waitFor([&] { return service->acks == 2; }, "duplicate ack missing");
            require(getBool(service->lastAck, "success"), "duplicate rejected");
            auto old = event;
            old["sequence"] = 0;
            old["status"] = "旧状态";
            service->event(old);
            co_await waitFor([&] { return service->acks == 3; }, "old ack missing");
            require(manager.runningActivities()[0]["status"] == "等待落子", "old event overwrote state");
            auto invalidEvent = event;
            invalidEvent["run_id"] = "another-run";
            service->event(invalidEvent);
            co_await waitFor([&] { return service->acks == 4; }, "invalid run ack missing");
            require(!getBool(service->lastAck, "success"), "mismatched run accepted");
            invalidEvent = event;
            invalidEvent["status"] = "同版本篡改";
            service->event(invalidEvent);
            co_await waitFor([&] { return service->acks == 5; }, "changed version ack missing");
            require(!getBool(service->lastAck, "success"), "same-version mutation accepted");
            service->close();
            co_await waitFor(
              [&] { return !getBool(manager.configuration()["servers"][0], "connected"); }, "disconnect not reflected");
            require(getStr(manager.runningActivities()[0], "status").find("状态未确认") != std::string::npos,
              "disconnect hid activity");
            require(!(co_await manager.refresh("chess")).has_value(), "offline synchronization accepted");
            require(!manager.uploadFile("chess", runId, pngBytes()), "offline upload accepted");
            event = changeSnapshot("断线期间轮到机器人");
            event["mode"] = "silent";
            auto replacement = connectService(endpoint);
            clients.push_back(replacement);
            co_await waitFor([&] { return replacement->ready.load(); }, "reconnect sync failed");
            replacement->event(event);
            co_await waitFor([&] { return replacement->acks == 1; }, "same-sequence recovery event lost");
            require(getBool(replacement->lastAck, "success"), "recovery rejected");
            // 新连接主动接管仍在线的旧连接。
            auto takeover = connectService(endpoint);
            clients.push_back(takeover);
            co_await waitFor([&] { return takeover->ready.load(); }, "takeover failed");
            require(getBool(manager.configuration()["servers"][0], "connected"), "old close cleared replacement");
            auto stopped = co_await manager.invoke("stop", "chess", json::object(), origin());
            require(
              getBool(stopped, "accepted") && !manager.runningActivities().empty(), "accepted stop treated as ended");
            event = changeSnapshot("已结束", false);
            event["mode"] = "silent";
            takeover->event(event);
            co_await waitFor([&] { return takeover->acks == 1; }, "finish not acknowledged");
            require(manager.runningActivities().empty(), "ended activity retained");
            require(!manager.uploadFile("chess", runId, pngBytes()), "ended run upload accepted");
            auto resurrected = event;
            resurrected["running"] = true;
            resurrected["sequence"] = getInt(event, "sequence") + 1;
            takeover->event(resurrected);
            co_await waitFor([&] { return takeover->acks == 2; }, "terminal run ack missing");
            require(
              !getBool(takeover->lastAck, "success") && manager.runningActivities().empty(), "ended run reactivated");
            // 已执行启动而未收到响应：只返回不确定，重连恢复，不重复发送启动。
            loseStartResponse = true;
            auto uncertain = co_await manager.invoke("start", "chess", json::object(), origin());
            require(!getBool(uncertain, "success", true), "lost response returned success");
            loseStartResponse = false;
            auto recovered = connectService(endpoint);
            clients.push_back(recovered);
            co_await waitFor([&] { return recovered->ready.load(); }, "uncertain start not recovered");
            require(
              manager.runningActivities().size() == 1 && callCount("start") == 2, "uncertain start retried or lost");
            auto crossRun = changeSnapshot("新运行不能使用旧图片");
            crossRun["attachments"] = json::array({{{"file_id", fileId}}});
            recovered->event(crossRun);
            co_await waitFor([&] { return recovered->acks == 1; }, "cross-run attachment ack missing");
            require(!getBool(recovered->lastAck, "success"), "old-run file ID accepted in new run");
            // 超时只查询状态，不重新发送有副作用的指令。
            unavailable = true;
            const auto beforeCommand = callCount("command");
            uncertain = co_await manager.invoke("command", "chess", json::object(), origin());
            unavailable = false;
            require(!getBool(uncertain, "success", true) && callCount("command") == beforeCommand + 1,
              "timeout retried command");
            co_await waitFor(
              [&] { return unregistered->closed.load(); }, "registration deadline did not close idle socket");
            // Token 热更新同时取消已注册及尚未注册连接，不丢失等待操作。
            auto pendingRegistration = connectService(endpoint, "pending", kToken, "", false);
            clients.push_back(pendingRegistration);
            co_await waitFor([&] { return pendingRegistration->attempted.load(); }, "pending handshake missing");
            unavailable = true;
            done = false;
            drogon::async_run([&]() -> drogon::Task<> {
                uncertain = co_await manager.invoke("command", "chess", json::object(), origin());
                done = true;
            });
            co_await waitFor([&] { return callCount("command") == beforeCommand + 2; }, "pending command not sent");
            const auto rotatedToken = std::string("rotated-fixture-secret");
            require(manager.saveToken(rotatedToken, store).has_value(), "token rotation failed");
            co_await waitFor([&] { return done.load(); }, "stop leaked waiter");
            co_await waitFor(
              [&] { return pendingRegistration->closed.load(); }, "rotation left unregistered connection alive");
            require(!manager.authorized(kToken) && manager.authorized(rotatedToken), "old token remains authorized");
            require(!getBool(uncertain, "success", true), "stop returned confirmed success");
            unavailable = false;
            auto rotated = connectService(endpoint, "chess", rotatedToken);
            clients.push_back(rotated);
            co_await waitFor([&] { return rotated->ready.load(); }, "new token cannot restore activity");
            unavailable = true;
            done = false;
            const auto beforeDelete = callCount("command");
            drogon::async_run([&]() -> drogon::Task<> {
                uncertain = co_await manager.invoke("command", "chess", json::object(), origin());
                done = true;
            });
            co_await waitFor([&] { return callCount("command") > beforeDelete; }, "delete fixture request missing");
            auto deleteRequest = drogon::HttpRequest::newHttpRequest();
            deleteRequest->setMethod(drogon::Delete);
            deleteRequest->setPath("/admin/api/activities");
            deleteRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            deleteRequest->setBody("{\"name\":\"chess\",\"force\":\"true\"}");
            const auto invalidDelete = co_await httpClient->sendRequestCoro(deleteRequest, 3);
            require(invalidDelete->statusCode() == drogon::k400BadRequest && !registered("chess").is_null(),
              "non-boolean force accepted");
            deleteRequest = drogon::HttpRequest::newHttpRequest();
            deleteRequest->setMethod(drogon::Delete);
            deleteRequest->setPath("/admin/api/activities");
            deleteRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            deleteRequest->setBody("{\"name\":\"chess\",\"force\":true}");
            const auto forced = co_await httpClient->sendRequestCoro(deleteRequest, 3);
            require(forced->statusCode() == drogon::k200OK, "force delete endpoint failed");
            co_await waitFor(
              [&] { return done.load() && rotated->closed.load(); }, "force delete left connection or waiter alive");
            require(!getBool(uncertain, "success", true) && registered("chess").is_null() &&
                      manager.runningActivities().empty(),
              "force delete retained runtime or confirmed pending operation");
            auto savedState = json{};
            std::ifstream("data/activity-state.json") >> savedState;
            require(!savedState.contains("chess") && store.getActivitiesConfig()["servers"].empty(),
              "force deletion not persisted");
            unavailable = false;
            auto rediscovered = connectService(endpoint, "chess", rotatedToken);
            clients.push_back(rediscovered);
            co_await waitFor([&] { return rediscovered->ready.load(); }, "deleted service cannot register again");
            require(!manager.removeService("chess", store), "ordinary deletion bypassed running guard");
            rediscovered->close();
            co_await waitFor(
              [&] { return !getBool(registered("chess"), "connected"); }, "offline fixture did not disconnect");
            require(manager.removeService("chess", store, true).has_value() && manager.runningActivities().empty(),
              "abandoned offline activity cannot be force deleted");
            require(
              manager.saveToken("", store).has_value() && !manager.authorized(rotatedToken) && !manager.authorized(""),
              "empty token did not close access");
            manager.stop();
            require(HttpTrace::instance().size() > 0, "WS traces missing");
            co_await testWorkflow(endpoint, root);
            exitCode = 0;
            std::cout << "All activity WebSocket and workflow tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        manager.stop();
        for (const auto &client: clients)
            client->close();
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), .1);
        drogon::app().quit();
    }
    auto runPeriodic(const std::string endpoint, const std::filesystem::path root, int &exitCode) -> drogon::Task<> {
        auto &manager = ActivityManager::instance();
        std::shared_ptr<MockService> service;
        try {
            require(Config::instance().initialize((root / "config.json").string()).has_value(),
              "periodic fixture config failed");
            require(manager.initialize(configuration()).has_value(), "periodic manager init failed");
            manager.start(*drogon::app().getLoop(), std::chrono::seconds{1});
            service = connectService(endpoint);
            co_await waitFor([&] { return service->ready.load(); }, "periodic service not ready");
            const auto started = co_await manager.invoke("start", "chess", json::object(), origin());
            require(started.contains("snapshot"), "periodic fixture start failed");
            const auto before = callCount("status");
            changeSnapshot("未推送的最新状态");
            co_await waitFor(
              [&] {
                  return getStr(registered("chess"), "status") == "未推送的最新状态" && callCount("status") > before;
              },
              "periodic query did not repair missed push");
            changeSnapshot("已结束", false);
            co_await waitFor(
              [&] { return manager.runningActivities().empty(); }, "periodic query did not detect finish");
            require(executorRequests == 0 && routerRequests == 0, "periodic query triggered model");
            exitCode = 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        manager.stop();
        if (service)
            service->close();
        co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), .1);
        drogon::app().quit();
    }
} // namespace

auto main(int argc, char **argv) -> int {
    const auto periodic = argc > 1 && std::string(argv[1]) == "--periodic";
    auto root = std::filesystem::temp_directory_path() / ("insoulforge-activities-" + drogon::utils::getUuid());
    std::filesystem::create_directories(root);
    // 单例持久化也隔离到临时运行目录，绝不接触项目 data。
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
    auto client = trantor::EventLoopThread{};
    client.run();
    int exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        client.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                if (periodic)
                    co_await runPeriodic(endpoint, root, exitCode);
                else
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
