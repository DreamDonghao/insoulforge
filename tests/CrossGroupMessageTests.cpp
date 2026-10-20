/// @brief 使用本地 OneBot 验证消息工具的目标校验、发送及记录归属。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/SessionStore.hpp>

namespace {
    using namespace insoulforge;
    struct Fixture {
        std::mutex mutex;
        std::vector<json> sends;
        std::atomic<i32> stickerQueries{0};
        std::atomic<i32> modelQueries{0};
        std::atomic_bool fail{false};
        std::atomic<i32> qzoneRight{1};

        auto messages() -> std::vector<json> {
            std::lock_guard lock(mutex);
            return sends;
        }
    } fixture;

    void require(const bool value, const char *message) {
        if (!value) throw std::runtime_error(message);
    }

    auto runTests(const std::string endpoint, i32 &exitCode) -> drogon::Task<> {
        try {
            auto &db = Database::instance();
            auto &config = Config::instance();
            require(db.initialize(":memory:").has_value(), "database initialization failed");
            config.oneBotTransport = "http";
            config.qqHttpHost = endpoint;
            config.router.baseUrl = config.executor.baseUrl = config.memory.baseUrl = endpoint;
            config.router.path = config.executor.path = config.memory.path = "/unexpected-model";
            config.router.model = config.executor.model = config.memory.model = "fixture";
            auto &agent = AgentSystem::instance();
            require(agent.initialize(db, "test-bot").has_value(), "agent initialization failed");
            auto &workflow = OneBotEventWorkflow::instance();
            require(workflow.initialize(db, config, agent).has_value(), "workflow initialization failed");
            auto &registry = ToolRegistry::instance();
            SessionStore::enableSession(200);
            SessionStore::enableSession(201);
            SessionStore::disableSession(201);
            require(!SessionStore::getSessionConfig(100).crossSessionSharing &&
                    !SessionStore::getSessionConfig(200).crossSessionSharing, "sharing fixture must be disabled");
            for (const auto &tool : registry.getTools({})) {
                const auto &definition = tool["function"];
                if (definition["name"] == "reply_and_continue" || definition["name"] == "send_sticker") {
                    require(definition["parameters"]["properties"]["group_id"]["type"] == "string" &&
                        definition["parameters"]["required"].size() == 1, "group_id must be optional string");
                }
            }
            const auto invalidIds = json::array({nullptr, false, 200, "", " ", "0", "-1", "+200", "200x", "200.0",
                "18446744073709551616", std::to_string(SessionId::fromPrivateUser(200)),
                std::to_string(SessionId::kSystemAccountId), "201", "999"});
            for (const auto *tool : {"reply_and_continue", "send_sticker"}) {
                for (const auto &id : invalidIds) {
                    const json args = {{"content", "test"}, {"name", "fixture"}, {"group_id", id}};
                    const auto result = co_await registry.executeTool(tool, args, {.sessionId = 100});
                    require(result.find("group_id") != std::string::npos || result.find("未在后台启用") != std::string::npos,
                        "invalid target not rejected");
                }
            }
            require(fixture.messages().empty() && fixture.stickerQueries == 0,
                "invalid target performed network requests");

            auto result = co_await registry.executeTool("reply_and_continue",
                {{"content", "cross-group text"}, {"group_id", "200"}}, {.sessionId = 100});
            require(result.find("目标群 200") != std::string::npos && result.find("消息已发送") != std::string::npos,
                "cross-group text result omitted destination or success");
            result = co_await registry.executeTool("send_sticker",
                {{"name", "fixture"}, {"group_id", "200"}}, {.sessionId = 100});
            require(result.find("目标群 200") != std::string::npos && result.find("已发送表情") != std::string::npos,
                "cross-group sticker did not send");
            require(!workflow.getSessionMessages(100) && workflow.getSessionMessages(200)->size() == 2,
                "cross-group messages were copied into the calling session");

            const auto privateSession = SessionId::fromPrivateUser(300);
            for (const auto session : {u64{100}, privateSession}) {
                result = co_await registry.executeTool("reply_and_continue", {{"content", "local text"}}, {.sessionId = session});
                require(result.find("消息已发送") != std::string::npos, "default text destination changed");
                result = co_await registry.executeTool("send_sticker", {{"name", "fixture"}}, {.sessionId = session});
                require(result.find("已发送表情") != std::string::npos, "default sticker destination changed");
            }
            result = co_await registry.executeTool("reply_and_continue",
                {{"content", "private-to-group"}, {"group_id", "200"}}, {.sessionId = privateSession});
            require(result.find("目标群 200") != std::string::npos, "private session cannot send to enabled group");
            const auto sent = fixture.messages();
            require(sent.size() == 7 && sent[0]["group_id"] == 200 && sent[0]["message"] == "cross-group text" &&
                sent[1]["group_id"] == 200 && getStr(sent[1], "message").find("sub_type=1") != std::string::npos &&
                sent[2]["group_id"] == 100 && sent[3]["group_id"] == 100 &&
                sent[4]["user_id"] == 300 && sent[5]["user_id"] == 300 && sent[6]["group_id"] == 200,
                "OneBot requests used incorrect destinations or sticker format");

            for (const auto right : {1, 4, 64}) {
                fixture.qzoneRight = right;
                json args = {{"content", "visibility fixture"}};
                if (right != 1) args["ugc_right"] = right;
                result = co_await registry.executeTool("send_qzone_msg", args, {.sessionId = 100});
                require(result.find("fixture-tid") != std::string::npos,
                    "qzone default or explicit visibility was not passed to OneBot");
            }
            fixture.fail = true;
            for (const auto *tool : {"reply_and_continue", "send_sticker"}) {
                result = co_await registry.executeTool(tool,
                    {{"content", "failed text"}, {"name", "fixture"}, {"group_id", "200"}}, {.sessionId = 100});
                require(result.find("目标群 200") != std::string::npos && result.find("未确认成功") != std::string::npos,
                    "failed send reported success or omitted target");
            }
            require(fixture.messages().size() == 9, "failed sends were automatically retried");
            config.oneBotTransport = "websocket";
            result = co_await registry.executeTool("reply_and_continue",
                {{"content", "unconfirmed"}, {"group_id", "200"}}, {.sessionId = 100});
            require(result.find("未确认成功") != std::string::npos && fixture.messages().size() == 9,
                "disconnected send retried via HTTP or reported success");
            const auto targetRecords = workflow.getSessionMessages(200);
            require(targetRecords && targetRecords->size() == 6 && targetRecords->back().contains("send_error") &&
                workflow.getSessionMessages(100)->size() == 2 && workflow.getSessionMessages(privateSession)->size() == 2,
                "failed or successful records were placed in wrong session");
            workflow.flushMessageListsToStorage();
            require(ChatRecordStore::getChatRecords(db, 200).size() == 6, "target records were not persisted");
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.05);
            require(fixture.modelQueries == 0, "outgoing message triggered automatic model reply or summary");
            exitCode = 0;
            std::cout << "All cross-group message tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
    }
}

auto main() -> int {
    const auto configPath = std::filesystem::temp_directory_path() /
        ("insoulforge-cross-group-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    require(Config::instance().initialize(configPath.string()).has_value(), "config initialization failed");
    drogon::app().setLogLevel(trantor::Logger::kWarn).addListener("127.0.0.1", 0);
    for (const auto *path : {"/send_group_msg", "/send_private_msg"}) {
        drogon::app().registerHandler(path,
            [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                json body;
                {
                    std::lock_guard lock(fixture.mutex);
                    fixture.sends.push_back(json::parse(request->body()));
                    body = fixture.fail ? json{{"status", "failed"}, {"retcode", 1200}, {"message", "fixture failure"}} :
                        json{{"status", "ok"}, {"data", {{"message_id", fixture.sends.size()}}}};
                }
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                response->setBody(body.dump());
                callback(response);
            }, {drogon::Post});
    }
    drogon::app().registerHandler("/fetch_custom_face_detail",
        [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            ++fixture.stickerQueries;
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(R"({"status":"ok","data":[{"desc":"fixture","url":"https://example.com/sticker.png"}]})");
            callback(response);
        }, {drogon::Post});
    drogon::app().registerHandler("/send_qzone_msg",
        [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            const auto params = json::parse(request->body());
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(params["ugc_right"] == fixture.qzoneRight.load() ?
                R"({"status":"ok","data":{"tid":"fixture-tid"}})" : R"({"status":"failed","retcode":400})");
            callback(response);
        }, {drogon::Post});
    drogon::app().registerHandler("/unexpected-model",
        [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            ++fixture.modelQueries;
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k400BadRequest);
            callback(response);
        }, {drogon::Post});
    trantor::EventLoopThread clientThread;
    clientThread.run();
    i32 exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        clientThread.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                co_await runTests(endpoint, exitCode);
                clientThread.getLoop()->quit();
            });
        });
    });
    drogon::app().run();
    clientThread.wait();
    Database::instance().close();
    std::filesystem::remove(configPath);
    return exitCode;
}
