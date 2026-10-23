/// @brief 使用本地 OneBot 验证消息工具的目标校验、发送及记录归属。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <drogon/drogon.h>
#include <openssl/evp.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/image/CharacterImageStore.hpp>
#include <workflow/agent/tools/image/ImageGenerationService.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/history/SharedConversationContext.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>
#include <workflow/session/SessionStore.hpp>

namespace {
    using namespace insoulforge;
    struct Fixture {
        std::mutex mutex;
        std::vector<json> sends;
        std::vector<json> imageRequests;
        std::atomic<i32> stickerQueries{0};
        std::atomic<i32> modelQueries{0};
        std::atomic_bool fail{false};
        std::atomic<i32> qzoneRight{1};
        std::atomic_bool failImage{false};

        auto images() -> std::vector<json> {
            std::lock_guard lock(mutex);
            return imageRequests;
        }

        auto messages() -> std::vector<json> {
            std::lock_guard lock(mutex);
            return sends;
        }
    } fixture;

    void require(const bool value, const char *message) {
        if (!value) throw std::runtime_error(message);
    }

    auto imageBytes() -> std::string {
        const std::string encoded =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9d1QAAAABJRU5ErkJggg==";
        std::string bytes(encoded.size() / 4 * 3, '\0');
        const auto length = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
            reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<int>(encoded.size()));
        bytes.resize(static_cast<size_t>(length) - 2);
        return bytes;
    }

    auto testImageGeneration(const std::string endpoint) -> drogon::Task<> {
        auto &config = Config::instance();
        config.imageGeneration.baseUrl = endpoint;
        config.imageGeneration.path = "/image-generation";
        config.imageGeneration.apiKey = "fixture";
        config.imageGeneration.model = "fixture";
        for (const auto &references : {std::vector<std::string>{}, std::vector<std::string>{"first"},
                 std::vector<std::string>{"first", "second", "third"}}) {
            const auto result = co_await ImageGenerationService::generate("test", "1024x1024", 500, references);
            require(result.source == "https://example.com/generated.png", "image response parsing changed");
            const auto request = fixture.images().back();
            require(request["size"] == "1024x1024", "image size was lost");
            if (references.empty()) {
                require(!request.contains("input_references"), "text generation unexpectedly attached references");
            } else {
                require(request["input_references"].size() == references.size(), "references were dropped");
                for (size_t index = 0; index < references.size(); ++index)
                    require(request["input_references"][index]["image_url"]["url"] == references[index],
                        "reference order changed");
            }
        }
        fixture.failImage = true;
        bool failed = false;
        try {
            co_await ImageGenerationService::generate("test", "1024x1024", 500);
        } catch (const std::exception &) { failed = true; }
        require(failed, "upstream rejection was silently ignored");
        fixture.failImage = false;

        auto &registry = ToolRegistry::instance();
        const json record = {{"message_id", "5001"}, {"assets", {{"images", {
            {{"source", {{"url", endpoint + "/reference.png"}}}},
            {{"source", {{"url", endpoint + "/reference.png"}}}}}}}}};
        ChatRecordStore::addChatRecord(500, "user", record.dump());
        const json args = {{"prompt", "参考图片"}, {"reference_images", {{{"message_id", "5001"}, {"image_index", 0}}}}};
        auto error = co_await registry.executeTool("generate_image", args, {.sessionId = 501});
        require(error.contains("未找到该会话"), "references leaked across sessions");
        json invalid = args;
        invalid["reference_images"][0]["image_index"] = 2;
        error = co_await registry.executeTool("generate_image", invalid, {.sessionId = 500});
        require(error.contains("没有可下载"), "missing source was silently ignored");

        // Isolate the relative character-image store from any developer data.
        const auto originalPath = std::filesystem::current_path();
        const auto temporary = std::filesystem::temp_directory_path() /
            ("insoulforge-image-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temporary);
        std::filesystem::current_path(temporary);
        require(!CharacterImageStore::save(imageBytes()), "character image fixture could not be saved");
        auto mixed = args;
        mixed["use_self_reference"] = true;
        mixed["reference_images"][0]["message_id"] = "5002";
        mixed["reference_images"].push_back({{"message_id", "5001"}, {"image_index", 1}});
        mixed["reference_images"].push_back(mixed["reference_images"][0]);
        auto snapshotRecord = record;
        snapshotRecord["message_id"] = "5002";
        const auto sentBefore = fixture.messages().size();
        const auto started = co_await registry.executeTool("generate_image", mixed,
            {.sessionId = 500, .messageSnapshot = json::array({snapshotRecord})});
        require(started.contains("已启动"), "mixed reference task did not start");
        for (int attempt = 0; attempt < 200 && fixture.messages().size() == sentBefore; ++attempt)
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.01);
        require(fixture.messages().size() == sentBefore + 1, "generated image was not delivered");
        const auto request = fixture.images().back();
        require(request["input_references"].size() == 3 &&
            getStr(request, "prompt").starts_with("第一张参考图是你的角色形象图"),
            "mixed references lost the character image or retained duplicate chat references");
        for (const auto &reference : request["input_references"])
            require(reference["image_url"]["url"] == *ImageDescriptionService::staticImageDataUrl(imageBytes()),
                "references were not converted to image data URLs");
        co_await drogon::sleepCoro(drogon::app().getLoop(), 0.02);
        std::filesystem::current_path(originalPath);
        std::filesystem::remove_all(temporary);

        // A failed download must release the task slot without sending a text-only generation request.
        auto broken = record;
        broken["assets"]["images"][0]["source"]["url"] = endpoint + "/missing.png";
        const auto imageRequestsBefore = fixture.images().size();
        const auto failedStart = co_await registry.executeTool("generate_image", args,
            {.sessionId = 502, .messageSnapshot = json::array({broken})});
        require(failedStart.contains("已启动"), "download failure fixture did not start");
        bool restarted = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto next = co_await registry.executeTool("generate_image", args,
                {.sessionId = 502, .messageSnapshot = json::array({record})});
            if (next.contains("已启动")) {
                restarted = true;
                break;
            }
            require(next.contains("在执行"), "failed download left an unexpected task state");
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.01);
        }
        require(restarted, "failed download did not release the task slot");
        for (int attempt = 0; attempt < 200 && fixture.messages().size() == sentBefore + 1; ++attempt)
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.01);
        require(fixture.messages().size() == sentBefore + 2 && fixture.images().size() == imageRequestsBefore + 1 &&
            fixture.images().back()["input_references"].size() == 1,
            "download failure generated an image or subsequent single-reference generation failed");
        co_await drogon::sleepCoro(drogon::app().getLoop(), 0.02);
    }

    auto testGroupMessageQuery() -> drogon::Task<> {
        auto &workflow = OneBotEventWorkflow::instance();
        auto &registry = ToolRegistry::instance();
        auto &config = Config::instance();
        const auto oldWindow = config.contextWindowLimit;
        config.contextWindowLimit = 3;
        SessionStore::enableSession(400);
        SessionStore::updateSessionName(400, "查询测试群");
        json expected = json::array();
        std::string longText;
        for (int index = 0; index < 250; ++index) longText += "测";
        for (int index = 0; index < 25; ++index) {
            json record = {{"time", "2020-01-01 00:00:00"},
                {"sender", {{"qq", "11"}, {"name", "Alice"}, {"card", "hidden card"}}},
                {"message_id", std::to_string(1000 + index)}, {"reply_to", "999"},
                {"memories", {"hidden memory"}}, {"tool_history", {{{"name", "hidden"}}}},
                {"segments", {{{"type", "text"}, {"text", index == 0 ? longText : std::to_string(index)}}}}};
            if (index == 24) {
                record["segments"].push_back({{"type", "image"}, {"image_index", 0}});
                record["assets"] = {{"images", {{{"recognition_status", "succeeded"}, {"description", "a cat"},
                    {"source", {{"url", "https://example.com/private.png"}}}}}}};
            }
            expected.push_back(*SharedConversationContext::projectMessage(record));
            workflow.appendDeliveredAssistantMessage(400, record);
        }
        const auto rawRecords = *workflow.getSessionMessages(400);
        auto excluded = rawRecords[1];
        excluded["message_id"] = "2001";
        excluded["sender"]["qq"] = std::to_string(SessionId::kSystemAccountId);
        workflow.appendSystemStatusMessage(400, excluded);
        excluded = rawRecords[1];
        excluded["message_id"] = "2002";
        excluded["send_error"] = "not delivered";
        workflow.appendAssistantExecutionRecord(400, excluded);
        excluded = rawRecords[1];
        excluded["message_id"] = "2003";
        excluded["record_type"] = "tool_execution";
        workflow.appendAssistantExecutionRecord(400, excluded);

        const auto before = workflow.getSessionMessages(400);
        require(before->size() == 28, "query fixture did not retain excluded records");
        constexpr auto caller = SessionId::fromPrivateUser(999);
        const json args = {{"group_id", "400"}};
        const auto result = co_await registry.executeTool("get_group_messages", args, {.sessionId = caller});
        const auto output = json::parse(result);
        require(output["session"] == json({{"type", "group"}, {"group_id", 400}, {"group_name", "查询测试群"}}) &&
            output["messages"] == expected && output["messages"].size() == 25,
            "query was limited by age, sharing count, context window, or returned wrong projection order");
        require(output["messages"][0]["truncated"] == true &&
            output["messages"][0]["segments"][0]["text"].get<std::string>().size() == 600 &&
            !result.contains("hidden") && !result.contains("message_id") && !result.contains("reply_to") &&
            !result.contains("image_index") && !result.contains("private.png") &&
            output["messages"][24]["segments"][1]["description"] == "a cat",
            "query leaked metadata or broke UTF-8 truncation or image summaries");
        require(workflow.getSessionMessages(400) == before && !workflow.getSessionMessages(caller),
            "query changed target records or created calling session");
        require(!SessionStore::getSessionConfig(400).crossSessionSharing &&
            !SessionStore::getSessionConfig(caller).crossSessionSharing,
            "query unexpectedly changed sharing flags");

        SessionStore::enableSession(401);
        ChatRecordStore::addChatRecord(Database::instance(), 401, "user", rawRecords[1].dump());
        const json emptyArgs = {{"group_id", "401"}};
        const auto empty = co_await registry.executeTool("get_group_messages", emptyArgs, {.sessionId = 100});
        require(json::parse(empty)["messages"].empty() && !workflow.getSessionMessages(401),
            "query read database history or created absent session");
        SessionStore::enableSession(402);
        auto filtered = rawRecords[1];
        filtered["send_error"] = "failed";
        workflow.appendAssistantExecutionRecord(402, filtered);
        const json filteredArgs = {{"group_id", "402"}};
        const auto onlyFiltered = co_await registry.executeTool("get_group_messages", filteredArgs, {.sessionId = 100});
        require(json::parse(onlyFiltered)["messages"].empty(), "filtered-only session must return an empty array");

        require(registry.saveBuiltinToolConfig("get_group_messages", {{"enabled", false}}).has_value(),
            "query tool missing builtin configuration");
        const auto disabled = co_await registry.executeTool("get_group_messages", args, {.sessionId = 100});
        require(disabled.contains("工具已关闭"), "disabled query tool was executed");
        require(registry.saveBuiltinToolConfig("get_group_messages", {{"enabled", true}}).has_value(),
            "query tool could not be enabled again");
        SessionStore::disableSession(400);
        const auto forbidden = co_await registry.executeTool("get_group_messages", args, {.sessionId = 100});
        require(forbidden.contains("未在后台启用") && !forbidden.contains("Alice"),
            "disabled target leaked retained records");
        config.contextWindowLimit = oldWindow;
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
                if (definition["name"] == "get_group_messages") {
                    require(definition["parameters"]["properties"]["group_id"]["type"] == "string" &&
                        definition["parameters"]["required"] == json::array({"group_id"}),
                        "query requires a string group_id");
                }
            }
            const auto invalidIds = json::array({nullptr, false, 200, "", " ", "0", "-1", "+200", "200x", "200.0",
                "18446744073709551616", std::to_string(SessionId::fromPrivateUser(200)),
                std::to_string(SessionId::kSystemAccountId), "201", "999"});
            for (const auto *tool : {"reply_and_continue", "send_sticker", "get_group_messages"}) {
                for (const auto &id : invalidIds) {
                    const json args = {{"content", "test"}, {"name", "fixture"}, {"group_id", id}};
                    const auto result = co_await registry.executeTool(tool, args, {.sessionId = 100});
                    require(result.find("group_id") != std::string::npos || result.find("未在后台启用") != std::string::npos,
                        "invalid target not rejected");
                }
            }
            require(fixture.messages().empty() && fixture.stickerQueries == 0,
                "invalid target performed network requests");
            const auto missing = co_await registry.executeTool("get_group_messages", json::object(), {.sessionId = 100});
            require(missing.contains("group_id"), "query silently defaulted missing target");
            co_await testGroupMessageQuery();
            require(fixture.messages().empty() && fixture.stickerQueries == 0 && fixture.modelQueries == 0,
                "memory query performed network or model requests");

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

            constexpr auto privateSession = SessionId::fromPrivateUser(300);
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
            fixture.fail = false;
            config.oneBotTransport = "http";
            co_await testImageGeneration(endpoint);
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
    drogon::app().registerHandler("/reference.png",
        [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setBody(imageBytes());
            callback(response);
        }, {drogon::Get});
    drogon::app().registerHandler("/image-generation",
        [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            {
                std::lock_guard lock(fixture.mutex);
                fixture.imageRequests.push_back(json::parse(request->body()));
            }
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            if (fixture.failImage) response->setStatusCode(drogon::k400BadRequest);
            response->setBody(R"({"data":[{"url":"https://example.com/generated.png"}]})");
            callback(response);
        }, {drogon::Post});
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
                const auto response = drogon::HttpResponse::newHttpResponse();
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
        [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) -> void {
            const auto params = json::parse(request->body());
          const auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(params["ugc_right"] == fixture.qzoneRight.load() ?
                R"({"status":"ok","data":{"tid":"fixture-tid"}})" : R"({"status":"failed","retcode":400})");
            callback(response);
        }, {drogon::Post});
    drogon::app().registerHandler("/unexpected-model",
        [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) -> void {
            ++fixture.modelQueries;
          const auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k400BadRequest);
            callback(response);
        }, {drogon::Post});
    trantor::EventLoopThread clientThread;
    clientThread.run();
    i32 exitCode = 1;
    drogon::app().registerBeginningAdvice([&] -> void {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        clientThread.getLoop()->queueInLoop([&, endpoint] -> void {
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
