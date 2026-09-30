/// @file MessageContractTests.cpp
/// @brief 消息链路的契约测试

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>

#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <chrono>
#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/maintenance/ConversationMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityStore.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/workflow/CommandProcessor.hpp>
#include <conversation/workflow/MessageList.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <conversation/workflow/OneBotEventNormalizer.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <llm/JevClient.hpp>
#include <llm/usage/UsageStore.hpp>
#include <media/ImageDescriptionStore.hpp>

namespace {
    using insoulforge::i32;
    using insoulforge::i64;
    using insoulforge::u64;

    i32 failures = 0;

    void check(const bool condition, const std::string_view expression, const std::string_view testName) {
        if (!condition) {
            std::cerr << "[FAIL] " << testName << ": " << expression << '\n';
            ++failures;
        }
    }

    void testNewWorkflowNormalizesOneBotEvent() {
        constexpr std::string_view kTestName = "new workflow OneBot event normalization";
        const insoulforge::json body = {
          {"post_type", "message"},
          {"time", 1'788'800'000},
          {"self_id", 42},
          {"message_type", "group"},
          {"group_id", 100},
          {"message_id", 7},
          {"sender", {{"user_id", 11}, {"nickname", "Alice"}}},
          {"message", {{{"type", "reply"}, {"data", {{"id", "6"}}}}, {{"type", "at"}, {"data", {{"qq", "42"}}}},
                        {{"type", "text"}, {"data", {{"text", "看图"}}}},
                        {{"type", "image"}, {"data", {{"file", "cat.jpg"}, {"url", "https://example.com/cat.jpg"}}}}}},
        };

        const auto record = insoulforge::OneBotEventNormalizer::normalize(body);
        check(record.has_value(), "message event is supported", kTestName);
        if (!record) {
            return;
        }
        check((*record)["sender"]["name"] == "Alice", "sender is normalized", kTestName);
        check((*record)["message_id"] == "7", "message ID is normalized to string", kTestName);
        check((*record)["session_id"] == 100, "group session ID is retained", kTestName);
        check((*record)["reply_to"] == "6", "reply ID is extracted", kTestName);
        check((*record)["segments"].size() == 3, "reply is not a content segment", kTestName);
        check((*record)["segments"][0]["type"] == "at", "at keeps segment order", kTestName);
        check((*record)["segments"][2]["image_index"] == 0, "image references asset by index", kTestName);
        check((*record)["assets"]["images"].size() == 1, "image source has one asset entry", kTestName);
        check((*record)["assets"]["images"][0]["source"]["url"] == "https://example.com/cat.jpg",
          "image URL stays in assets", kTestName);
        check(insoulforge::QQNameDirectory::getName(11) == "Alice", "sender name is recorded", kTestName);
        check(insoulforge::SessionId::toStorageTarget(insoulforge::SessionId::fromPrivateUser(11)).first == "private",
          "private session storage target is preserved", kTestName);

        insoulforge::json selfEvent = body;
        selfEvent["message_id"] = 8;
        selfEvent["sender"] = {{"user_id", 42}, {"nickname", "Bot"}};
        const auto selfRecord = insoulforge::OneBotEventNormalizer::normalize(std::move(selfEvent));
        check(selfRecord.has_value(), "bot echo is supported", kTestName);
        if (selfRecord) {
            check((*selfRecord)["sender"]["qq"] == "self", "bot echo is marked as self", kTestName);
        }

        const auto unsupported = insoulforge::OneBotEventNormalizer::normalize({{"post_type", "meta_event"}});
        check(!unsupported, "unsupported event is ignored", kTestName);
    }

    void testNewWorkflowNormalizesOneBotNotices() {
        constexpr std::string_view kTestName = "new workflow OneBot notice normalization";
        const auto poke =
          insoulforge::OneBotEventNormalizer::normalize({{"post_type", "notice"}, {"notice_type", "notify"},
            {"sub_type", "poke"}, {"time", 1'788'800'000}, {"self_id", 42}, {"user_id", 11}, {"target_id", 42}});
        check(poke.has_value(), "poke notice is supported", kTestName);
        if (poke) {
            check((*poke)["sender"]["qq"] == "11", "poke actor becomes sender", kTestName);
            check((*poke)["session_id"] == (1ULL << 63 | 11ULL), "private poke has private session ID", kTestName);
            check((*poke)["segments"][0]["type"] == "poke", "poke has semantic segment", kTestName);
            check((*poke)["segments"][0]["target"]["qq"] == "42", "poke target is retained", kTestName);
            check((*poke)["segments"][0]["direction"] == "inbound", "poke direction is normalized", kTestName);
        }

        const auto membership =
          insoulforge::OneBotEventNormalizer::normalize({{"post_type", "notice"}, {"notice_type", "group_decrease"},
            {"sub_type", "kick"}, {"time", 1'788'800'000}, {"group_id", 100}, {"user_id", 11}, {"operator_id", 12}});
        check(membership.has_value(), "membership notice is supported", kTestName);
        if (membership) {
            check((*membership)["segments"][0]["type"] == "member_event", "membership has semantic segment", kTestName);
            check((*membership)["segments"][0]["action"] == "leave", "leave action is normalized", kTestName);
            check((*membership)["segments"][0]["reason"] == "kick", "membership reason is retained", kTestName);
            check((*membership)["segments"][0]["operator"]["qq"] == "12", "membership operator is retained", kTestName);
        }
    }

    void testNewWorkflowDetectsCommands() {
        constexpr std::string_view kTestName = "new workflow command detection";
        auto &config = insoulforge::Config::instance();
        const u64 originalSelfId = config.selfQQNumber;
        config.selfQQNumber = 42;

        const insoulforge::json groupCommand = {{"session_id", 100},
          {"segments", {{{"type", "at"}, {"target", {{"qq", "42"}}}}, {{"type", "text"}, {"text", " /status"}}}}};
        check(insoulforge::CommandProcessor::isCommand(groupCommand), "bot mention enables group command", kTestName);

        const insoulforge::json groupMessage = {
          {"session_id", 100}, {"segments", {{{"type", "text"}, {"text", "/status"}}}}};
        check(!insoulforge::CommandProcessor::isCommand(groupMessage), "group command needs bot mention", kTestName);

        const insoulforge::json privateCommand = {
          {"session_id", 1ULL << 63 | 11ULL}, {"segments", {{{"type", "text"}, {"text", "\t/help"}}}}};
        check(insoulforge::CommandProcessor::isCommand(privateCommand), "private command needs no mention", kTestName);

        config.selfQQNumber = originalSelfId;
    }

    void testMessageRecordSemanticQueries() {
        constexpr std::string_view kTestName = "message record semantic queries";
        const insoulforge::json modernMention = {
          {"sender", {{"qq", "self"}}},
          {"segments", insoulforge::json::array(
                         {{{"type", "at"}, {"target", {{"qq", "42"}}}}, {{"type", "text"}, {"text", "hello"}}})},
        };
        const insoulforge::json legacyMention = {
          {"sender", {{"qq", std::to_string(insoulforge::SessionId::kSystemAccountId)}}},
          {"text", "legacy text"},
          {"segments", insoulforge::json::array({{{"type", "at"}, {"qq", "42"}}})},
        };

        check(insoulforge::MessageRecord::extractText(modernMention) == "hello", "extracts ordered text segments",
          kTestName);
        check(insoulforge::MessageRecord::extractText(legacyMention).empty(),
          "does not mix legacy text when segments exist", kTestName);
        const insoulforge::json legacyText = {{"text", "legacy text"}};
        check(
          insoulforge::MessageRecord::extractText(legacyText) == "legacy text", "falls back to legacy text", kTestName);
        check(insoulforge::MessageRecord::mentions(modernMention, 42), "matches modern mention target", kTestName);
        check(insoulforge::MessageRecord::mentions(legacyMention, 42), "matches legacy mention target", kTestName);
        check(
          insoulforge::MessageRecord::hasSegmentType(modernMention, "at"), "finds semantic segment type", kTestName);
        check(insoulforge::MessageRecord::isAssistant(modernMention), "recognizes assistant sender", kTestName);
        check(insoulforge::MessageRecord::isSystem(legacyMention), "recognizes system sender", kTestName);
    }

    void testNewWorkflowRouterHardRules() {
        constexpr std::string_view kTestName = "new workflow router hard rules";
        auto &config = insoulforge::Config::instance();
        const u64 originalSelfId = config.selfQQNumber;
        config.selfQQNumber = 42;

        const insoulforge::json mentionSnapshot =
          insoulforge::json::array({{{"message_id", "mention"}, {"sender", {{"qq", "11"}}},
            {"segments", insoulforge::json::array(
                           {{{"type", "at"}, {"target", {{"qq", "42"}}}}, {{"type", "text"}, {"text", "在吗"}}})}}});
        const auto mention = drogon::sync_wait(insoulforge::MessageRouter::route(100, "mention", mentionSnapshot));
        check(mention.shouldReply, "bot mention replies without LLM", kTestName);
        check(mention.isPriority, "bot mention is priority", kTestName);

        const insoulforge::json shortSnapshot = insoulforge::json::array({{{"message_id", "short"},
          {"sender", {{"qq", "11"}}}, {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "嗯"}}})}}});
        const auto shortMessage = drogon::sync_wait(insoulforge::MessageRouter::route(100, "short", shortSnapshot));
        check(!shortMessage.shouldReply, "short group message skips without LLM", kTestName);

        auto assistantAfterMention = mentionSnapshot;
        assistantAfterMention.push_back(
          {{"message_id", "assistant"}, {"sender", {{"qq", "self"}}}, {"segments", insoulforge::json::array()}});
        const auto queuedMention =
          drogon::sync_wait(insoulforge::MessageRouter::route(100, "mention", assistantAfterMention));
        check(queuedMention.shouldReply, "queued bot mention replies despite trailing assistant message", kTestName);

        config.selfQQNumber = originalSelfId;
    }

    void testJevUnconfiguredFallsBackToExistingBehavior() {

        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        constexpr std::string_view kTestName = "jev unconfigured falls back to existing behavior";
        // 测试进程不加载配置，config.jev 全空，JevClient::isConfigured 为 false，全程不发网络。
        // 未配置环境下兜底返回 REPLY "Router 请求失败"（fail-open），断言按此方向写。
        const insoulforge::json plainSnapshot = insoulforge::json::array({{{"message_id", "plain"},
          {"sender", {{"qq", "11"}}},
          {"segments",
            insoulforge::json::array({{{"type", "text"}, {"text", "今天天气的大家都在干什么"}}})}}});
        const auto fallback = drogon::sync_wait(insoulforge::MessageRouter::route(100, "plain", plainSnapshot));
        check(fallback.shouldReply, "unconfigured jev falls back to existing fail-open path", kTestName);
        check(fallback.reason == "Router 请求失败", "fallback reason is unchanged", kTestName);
    }

    /// @brief 读取 mock 服务地址；未设置环境变量时返回 nullopt
    /// @details Jev 优先路径的用例要发真实 HTTP，依赖外部 mock 进程。默认构建不跑
    ///          该用例，ctest 在无 mock 环境下保持零网络。
    std::optional<std::string> jevMockBaseUrl() {
        const char *raw = std::getenv("INSOULFORGE_JEV_MOCK_BASE_URL");
        if (raw == nullptr || *raw == '\0') {
            return std::nullopt;
        }
        return std::string(raw);
    }

    /// @brief 在后台线程跑 drogon 事件循环，析构时停循环并 join
    /// @details HttpUtil::send() 经 drogon::HttpClient::newHttpClient() 创建客户端，该客户端
    ///          绑定 app() 的事件循环；循环未运行时请求不会被派发。主线程用
    ///          drogon::sync_wait() 驱动协程，网络 IO 在本线程的循环上完成。
    class ScopedDrogonLoop {
    public:
        ScopedDrogonLoop() {
            m_thread = std::thread([] { drogon::app().run(); });
            while (!drogon::app().getLoop()->isRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        ScopedDrogonLoop(const ScopedDrogonLoop &) = delete;
        ScopedDrogonLoop &operator=(const ScopedDrogonLoop &) = delete;
        ScopedDrogonLoop(ScopedDrogonLoop &&) = delete;
        ScopedDrogonLoop &operator=(ScopedDrogonLoop &&) = delete;

        ~ScopedDrogonLoop() {
            drogon::app().quit();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

    private:
        std::thread m_thread;
    };

    /// @brief 路由一条普通群消息，指定 mock 的响应路径
    /// @param mockBaseUrl mock 服务地址
    /// @param path mock 路径，决定本次返回的 action
    /// @details 快照只含一条非 @提及、非系统、超过 2 个字的群消息，以绕过全部 5 条
    ///          硬规则，进入 Jev 优先分支。
    insoulforge::RouterDecision routeViaMockJev(const std::string &mockBaseUrl, const std::string &path) {
        auto &config = insoulforge::Config::instance();
        config.jev.baseUrl = mockBaseUrl;
        config.jev.path = path;
        config.jev.model = "typesafe/jev-1.13";
        config.jev.apiKey = "mock-key"; // isConfigured 要求四者均非空

        const insoulforge::json snapshot = insoulforge::json::array({{{"message_id", "p1"},
          {"sender", {{"qq", "11"}, {"name", "Alice"}}},
          {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "大家周末都去哪玩"}}})}}});
        return drogon::sync_wait(insoulforge::MessageRouter::route(100, "p1", snapshot));
    }

    /// @brief Jev 优先路径的端到端用例（需外部 mock 进程）
    /// @details 路径与响应的对应关系定义在 tests/mock/jev_mock_server.py，两者同时修改。
    ///          本用例走真实 HTTP 与真实计费写库，故需先建内存库。
    void testRouteWithMockJev() {
        constexpr std::string_view kTestName = "route with mock jev";
        const auto mockBaseUrl = jevMockBaseUrl();
        if (!mockBaseUrl) {
            std::cout << "[SKIP] " << kTestName << ": INSOULFORGE_JEV_MOCK_BASE_URL 未设置\n";
            return;
        }

        auto &config = insoulforge::Config::instance();
        const insoulforge::LLMApiConfig originalJev = config.jev;
        const u64 originalSelfId = config.selfQQNumber;
        config.selfQQNumber = 42;

        // JevClient::requestSystemOne() 成功后走 logUsage()，写入 llm_usage 表
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        const ScopedDrogonLoop loop;

        const auto reply = routeViaMockJev(*mockBaseUrl, "/reply");
        check(reply.shouldReply, "jev reply label yields REPLY", kTestName);
        check(reply.reason == "Jev 优先判定", "jev decision carries its own reason", kTestName);
        check(reply.tone == "serious", "jev tone label maps into decision", kTestName);
        check(reply.maxLength == 50, "jev maxLength label parses to integer", kTestName);
        check(!reply.isPrivate, "group session type is applied", kTestName);

        const auto skip = routeViaMockJev(*mockBaseUrl, "/skip");
        check(!skip.shouldReply, "jev skip label yields SKIP", kTestName);
        check(skip.reason == "Jev 优先判定", "jev skip carries the jev reason", kTestName);

        // unclear / 非 200 / answers 为空 三条路径均转入 LLM 兜底；测试环境未配置 router
        // 模型，兜底请求失败后 fail-open 返回 REPLY "Router 请求失败"。
        for (const std::string_view path: {"/unclear", "/error", "/malformed"}) {
            const auto fallback = routeViaMockJev(*mockBaseUrl, std::string(path));
            check(fallback.reason == "Router 请求失败",
              "jev abstention falls back to the LLM path", kTestName);
            check(fallback.shouldReply, "fallback keeps fail-open semantics", kTestName);
        }

        config.jev = originalJev;
        config.selfQQNumber = originalSelfId;
        database.close();
    }

    void testClassifyJevChoice() {
        constexpr std::string_view kTestName = "classify jev choice";
        using Action = insoulforge::RouterDecision::Action;

        const auto skip = insoulforge::MessageRouter::classifyJevChoice("skip");
        check(skip.has_value() && *skip == Action::SKIP, "skip label maps to SKIP", kTestName);
        const auto reply = insoulforge::MessageRouter::classifyJevChoice("reply");
        check(reply.has_value() && *reply == Action::REPLY, "reply label maps to REPLY", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("unclear").has_value(),
          "unclear label falls back to nullopt", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("other").has_value(),
          "unknown label falls back to nullopt", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("").has_value(),
          "empty label falls back to nullopt", kTestName);
    }

    void testJevClientChoiceAndConfidenceParsing() {
        constexpr std::string_view kTestName = "jev client choice and confidence parsing";
        const std::vector<std::string_view> validLabels{"skip", "reply", "unclear"};

        const insoulforge::json ok = {
          {"model", "typesafe/jev-1.13"},
          {"answers", {{"action", {{"choice", "reply"}, {"confidence", 0.82}}}}},
        };
        const auto choice = insoulforge::JevClient::readChoice(ok, "action", validLabels);
        check(choice.has_value() && *choice == "reply", "reads winning choice label", kTestName);
        const auto confidence = insoulforge::JevClient::readConfidence(ok, "action");
        check(confidence.has_value() && *confidence > 0.81 && *confidence < 0.83, "reads confidence", kTestName);

        check(!insoulforge::JevClient::readChoice(ok, "missing", validLabels).has_value(),
          "unknown question name yields nullopt", kTestName);
        check(!insoulforge::JevClient::readChoice(insoulforge::json{{"answers", insoulforge::json::object()}},
                 "action", validLabels)
                 .has_value(),
          "empty answers yields nullopt", kTestName);
        check(!insoulforge::JevClient::readChoice(insoulforge::json::object(), "action", validLabels).has_value(),
          "missing answers yields nullopt", kTestName);
        check(!insoulforge::JevClient::readChoice(
                 insoulforge::json{{"answers", {{"action", {{"choice", 42}}}}}}, "action", validLabels)
                 .has_value(),
          "wrong answer type yields nullopt", kTestName);
        check(!insoulforge::JevClient::readChoice(
                 insoulforge::json{{"answers", {{"action", {{"choice", "other"}}}}}}, "action", validLabels)
                 .has_value(),
          "label outside valid set yields nullopt", kTestName);

        check(!insoulforge::JevClient::readConfidence(
                 insoulforge::json{{"answers", {{"action", {{"choice", "reply"}, {"confidence", 1.5}}}}}}, "action")
                 .has_value(),
          "out of range confidence yields nullopt", kTestName);
        check(!insoulforge::JevClient::readConfidence(
                 insoulforge::json{{"answers", {{"action", {{"choice", "reply"}}}}}}, "action")
                 .has_value(),
          "missing confidence yields nullopt", kTestName);
        check(!insoulforge::JevClient::readConfidence(insoulforge::json::object(), "action").has_value(),
          "missing answers yields nullopt for confidence", kTestName);
    }

    void testRouterWindowStartIndexIsBatchedNotSliding() {
        constexpr std::string_view kTestName = "router window start index is batched not sliding";
        auto &config = insoulforge::Config::instance();
        const i32 originalTrigger = config.routerWindowTriggerCount;
        const i32 originalKeep = config.routerWindowKeepCount;
        config.routerWindowTriggerCount = 20;
        config.routerWindowKeepCount = 10;

        // keep=10, slide=10 时窗口大小 = 10 + size % 10，因此 size 为 10 的整数倍时窗口收回到 keep。
        // 以下用例锁住窗口的周期性：同一批次内起始下标保持不变，跨批次边界时前缀前移。
        check(insoulforge::MessageRouter::windowStartIndex(30) == 20,
          "window resets to keep at period boundary", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(35) == 20,
          "window grows within a period", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(39) == 20,
          "window keeps a stable prefix within a period", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(40) == 30,
          "next period starts a new prefix", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(5) == 0,
          "short snapshot starts at zero", kTestName);

        config.routerWindowTriggerCount = originalTrigger;
        config.routerWindowKeepCount = originalKeep;
    }

    void testMessageRecordProjectionHidesImageSources() {
        constexpr std::string_view kTestName = "message record projection";
        const insoulforge::json record = {
          {"time", "2026-09-07 17:22:05"},
          {"sender", {{"name", "Alice"}, {"qq", "11"}}},
          {"message_id", "7"},
          {"segments", {{{"type", "text"}, {"text", "看这张图"}}, {{"type", "image"}, {"image_index", 0}}}},
          {"assets", {{"images", {{{"recognition_status", "succeeded"}, {"description", "一只蓝色的猫"},
                                   {"source", {{"file", "cat.jpg"}, {"url", "https://example.com/cat.jpg"}}}}}}}},
          {"memories", {{{"id", 1}, {"content", "用户偏好简洁回答"}, {"similarity", 0.8}}}},
        };

        const insoulforge::json projected = insoulforge::MessageRecord::projectForAgent(record);
        check(!projected.contains("assets"), "agent projection excludes assets", kTestName);
        check(projected["segments"][1]["image_index"] == 0, "image index remains stable", kTestName);
        check(
          projected["segments"][1]["description"] == "一只蓝色的猫", "image description remains visible", kTestName);
        check(
          !projected.dump().contains("https://example.com/cat.jpg"), "agent projection excludes image URL", kTestName);
        check(projected["memories"] == insoulforge::json::array({"用户偏好简洁回答"}),
          "agent projection keeps recalled memory content", kTestName);

        const auto source = insoulforge::MessageRecord::findImageSource(record, 0);
        check(source && source->file == "cat.jpg", "tool can resolve image source", kTestName);
        check(insoulforge::MessageRecord::extractRecallText(record).contains("一只蓝色的猫"),
          "image description participates in recall", kTestName);

        const insoulforge::json legacyRecord = {
          {"message_id", "8"},
          {"segments", {{{"type", "image"}, {"file", "legacy.jpg"}, {"url", "https://example.com/legacy.jpg"},
                         {"recognition_status", "succeeded"}, {"description", "旧版图片"}}}},
          {"images", {{{"file", "legacy.jpg"}, {"url", "https://example.com/legacy.jpg"},
                       {"recognition_status", "succeeded"}, {"description", "旧版图片"}}}},
        };
        const insoulforge::json legacyProjection = insoulforge::MessageRecord::projectForAgent(legacyRecord);
        check(!legacyProjection.dump().contains("https://example.com/legacy.jpg"),
          "legacy projection excludes image URL", kTestName);
        const auto legacySource = insoulforge::MessageRecord::findImageSource(legacyRecord, 0);
        check(legacySource && legacySource->file == "legacy.jpg", "tool resolves legacy image source", kTestName);
    }

    void testAssistantStickerRecordKeepsOnlyName() {
        constexpr std::string_view kTestName = "assistant sticker record";
        const insoulforge::json record = insoulforge::MessageRecord::createAssistantRecord(
          "Bot(我)", 7, "[CQ:image,file=https://example.com/sticker.jpg,sub_type=1,summary=嘲讽]");
        check(record["segments"].size() == 1, "sticker has one semantic segment", kTestName);
        check(record["segments"][0]["type"] == "sticker", "sticker segment type", kTestName);
        check(record["segments"][0]["name"] == "嘲讽", "sticker summary becomes name", kTestName);
        check(
          !record.dump().contains("https://example.com/sticker.jpg"), "sticker record excludes CQ source", kTestName);
    }

    void testSchemaMigration() {
        constexpr std::string_view kTestName = "schema migration";
        sqlite3 *db = nullptr;
        check(sqlite3_open(":memory:", &db) == SQLITE_OK, "opens in-memory database", kTestName);
        if (!db)
            return;

        sqlite3_exec(db, "PRAGMA user_version = 6", nullptr, nullptr, nullptr);
        insoulforge::SchemaMigrator::migrate(db);
        sqlite3_stmt *stmt = nullptr;
        const i32 prepareResult =
          sqlite3_prepare_v2(db, "SELECT sampled_frame_count FROM image_description_cache LIMIT 1", -1, &stmt, nullptr);
        check(prepareResult == SQLITE_OK, "v6 migration creates sampled frame count column", kTestName);
        sqlite3_finalize(stmt);
        const i32 jobPrepareResult = sqlite3_prepare_v2(
          db, "SELECT messages, attempt_count FROM memory_maintenance_jobs LIMIT 1", -1, &stmt, nullptr);
        check(jobPrepareResult == SQLITE_OK, "migration creates durable memory maintenance jobs", kTestName);
        sqlite3_finalize(stmt);
        const i32 affinityJobPrepareResult = sqlite3_prepare_v2(
          db, "SELECT messages, attempt_count FROM affinity_maintenance_jobs LIMIT 1", -1, &stmt, nullptr);
        check(affinityJobPrepareResult == SQLITE_OK, "migration creates durable affinity maintenance jobs", kTestName);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    void testMemoryMaintenanceStore() {
        constexpr std::string_view kTestName = "memory maintenance store";
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        const insoulforge::json messages = {{{"message_id", "1"}, {"segments", insoulforge::json::array()}}};
        const i64 firstJob = insoulforge::MemoryMaintenanceStore::enqueue(100, messages, insoulforge::json::array(), 2);
        const auto pending = insoulforge::MemoryMaintenanceStore::pendingSessionIds();
        check(pending == std::vector<u64>{100}, "pending session survives storage boundary", kTestName);
        const auto first = insoulforge::MemoryMaintenanceStore::next(100);
        check(first && first->id == firstJob && first->messages == messages, "loads oldest persisted job", kTestName);

        insoulforge::MemoryMaintenanceStore::incrementAttempt(firstJob);
        const auto retried = insoulforge::MemoryMaintenanceStore::next(100);
        check(retried && retried->attemptCount == 1, "persists retry count", kTestName);

        insoulforge::MemoryMaintenanceStore::complete(firstJob, 100, std::string("近期状态\n"), {});
        check(!insoulforge::MemoryMaintenanceStore::next(100), "completed job is no longer executable", kTestName);
        check(insoulforge::MemoryMaintenanceStore::takeCompleted(100) == 2,
          "completed job reports its deferred removal count", kTestName);
        check(insoulforge::MemoryStore::getShortTermMemory(100) == "近期状态\n", "completion commits short-term memory",
          kTestName);

        const i64 secondJob =
          insoulforge::MemoryMaintenanceStore::enqueue(100, messages, insoulforge::json::array(), 0);
        const insoulforge::PreparedLongTermMemory longTerm{
          .content = "Alice 喜欢 C++", .embedding = {0.1F, 0.2F}, .replacedIds = {}};
        insoulforge::MemoryMaintenanceStore::complete(secondJob, 100, std::nullopt, {longTerm});
        check(insoulforge::LongTermMemoryStore::countMemories(100) == 1,
          "completion commits long-term memory in the same transaction", kTestName);
        database.close();
    }

    void testConversationMaintenanceStore() {
        constexpr std::string_view kTestName = "conversation maintenance store";
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        const insoulforge::json messages = {{{"message_id", "1"}, {"segments", insoulforge::json::array()}}};
        const insoulforge::json context = {{{"message_id", "2"}, {"segments", insoulforge::json::array()}}};
        insoulforge::ConversationMaintenanceStore::enqueue(100, messages, context);

        const auto memoryJob = insoulforge::MemoryMaintenanceStore::next(100);
        const auto affinityJob = insoulforge::AffinityMaintenanceStore::next(100);
        check(memoryJob && memoryJob->messages == messages && memoryJob->contextMessages == context,
          "creates memory task with its summary context", kTestName);
        check(affinityJob && affinityJob->messages == messages, "creates affinity task from the same batch", kTestName);

        insoulforge::AffinityMaintenanceStore::complete(affinityJob->id, 100, {{11, 3}});
        check(!insoulforge::AffinityMaintenanceStore::next(100), "completed affinity task is removed", kTestName);
        check(insoulforge::AffinityStore::getAffinityMap(100)[11] == 3,
          "completion applies affinity changes atomically", kTestName);
        database.close();
    }

    void testImageDescriptionCacheStore() {
        constexpr std::string_view kTestName = "image description cache store";
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");
        insoulforge::ConfigStore::initialize("data/message-contract-test-config.json");

        insoulforge::ImageDescriptionStore::upsert("hash", "vision-model", 1, "gif", true, "角色挥手", 16);
        const auto succeeded = insoulforge::ImageDescriptionStore::find("hash", "vision-model", 1);
        check(succeeded && succeeded->succeeded, "stores successful description", kTestName);
        check(succeeded && succeeded->description == "角色挥手", "stores description", kTestName);
        check(succeeded && succeeded->sampledFrameCount == 16, "stores sampled frame count", kTestName);

        insoulforge::ImageDescriptionStore::upsert("hash", "vision-model", 1, "gif", false, "", 0);
        const auto failed = insoulforge::ImageDescriptionStore::find("hash", "vision-model", 1);
        check(failed && !failed->succeeded, "updates cached failure", kTestName);
        check(insoulforge::ImageDescriptionStore::clearAll() == 1, "clears all cached descriptions", kTestName);
        check(!insoulforge::ImageDescriptionStore::find("hash", "vision-model", 1), "cleared entry is unavailable",
          kTestName);

        insoulforge::ConfigStore::saveLLMConfig(
          "image", {{"apiKey", "key"}, {"baseUrl", "https://example.com"}, {"path", "/v1/chat/completions"},
                     {"model", "vision-model"}, {"maxTokens", 1536}, {"temperature", 0.4}, {"topP", 0.8},
                     {"reasoningEffort", ""}});
        insoulforge::Config::instance().loadFromStorage();
        check(insoulforge::Config::instance().imageParams.maxTokens == 1536, "loads configured image max tokens",
          kTestName);
        database.close();
    }

    void testUsageSummaryUsesLatestRoleModel() {
        constexpr std::string_view kTestName = "usage summary model selection";
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        insoulforge::UsageStore::addUsageRecord("router", "old-model", 10, 5, 15, 0);
        insoulforge::UsageStore::addUsageRecord("router", "current-model", 20, 10, 30, 0);
        const insoulforge::json summary = insoulforge::UsageStore::getUsageSummary(30);
        const insoulforge::json &roles = summary["by_role"];
        check(roles.size() == 1, "aggregates calls for the same role", kTestName);
        check(roles[0]["model"] == "current-model", "uses most recently called model instead of lexical maximum",
          kTestName);
        check(roles[0]["total"] == 45, "preserves aggregated token count", kTestName);
        database.close();
    }

    void testMessageListSnapshotsAndPersistence() {
        constexpr std::string_view kTestName = "new workflow message list";
        auto &database = insoulforge::Database::instance();
        database.initialize(":memory:");

        auto &config = insoulforge::Config::instance();
        const i32 originalContextLimit = config.contextWindowLimit;
        const i32 originalTriggerCount = config.memorySummaryTriggerCount;
        const i32 originalBatchSize = config.memorySummaryBatchSize;
        const i32 originalSummaryContextCount = config.memorySummaryContextCount;
        config.contextWindowLimit = 2;
        config.memorySummaryTriggerCount = 3;
        config.memorySummaryBatchSize = 2;
        config.memorySummaryContextCount = 1;

        const auto makeMessage = [](const i32 id) -> insoulforge::json {
            insoulforge::json message;
            message["message_id"] = std::to_string(id);
            message["sender"] = {{"qq", "11"}, {"name", "Alice"}};
            message["segments"] = {{{"type", "text"}, {"text", "message " + std::to_string(id)}}};
            message["assets"]["images"] = {{{"source", {{"url", "https://example.com/image.jpg"}}}}};
            return message;
        };

        insoulforge::MessageList messages(100);
        const auto first = messages.append(makeMessage(1));
        static_cast<void>(messages.append(makeMessage(2)));
        const auto third = messages.append(makeMessage(3));
        const auto fourth = messages.append(makeMessage(4));

        check(first->messageSnapshot.size() == 1, "first snapshot stays immutable after later appends", kTestName);
        check(third->summaryBatch.has_value(), "reaches trigger by reserving a summary batch", kTestName);
        check(third->summaryBatch->messages.size() == 2, "summary batch has configured size", kTestName);
        check(
          third->summaryBatch->messages[0]["message_id"] == "1", "summary batch keeps chronological order", kTestName);
        check(third->summaryBatch->contextMessages.size() == 1, "summary batch includes configured context", kTestName);
        check(!fourth->summaryBatch, "does not create another summary while one is pending", kTestName);
        check(fourth->messageSnapshot.size() == 2, "snapshot retains configured recent window", kTestName);
        check(
          fourth->messageSnapshot[0]["message_id"] == "3", "snapshot starts at configured context limit", kTestName);
        check(fourth->messageSnapshot[1]["assets"]["images"][0]["source"]["url"] == "https://example.com/image.jpg",
          "complete image source remains available to tools", kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(100).empty(), "runtime append does not write database",
          kTestName);

        insoulforge::MemoryMaintenanceStore::enqueue(100, third->summaryBatch->messages,
          third->summaryBatch->contextMessages, third->summaryBatch->messages.size());
        const auto job = insoulforge::MemoryMaintenanceStore::next(100);
        check(job.has_value(), "reserved summary batch is persistable", kTestName);
        insoulforge::MemoryMaintenanceStore::complete(job->id, 100, std::nullopt, {});
        static_cast<void>(messages.removeCompletedSummaryMessages());
        check(messages.snapshot().size() == 2, "completed summary deletes only its oldest batch", kTestName);
        check(messages.snapshot()[0]["message_id"] == "3", "completed summary retains newer messages", kTestName);

        messages.flushToStorage();
        const auto restoredRecords = insoulforge::ChatRecordStore::getChatRecords(100);
        check(restoredRecords.size() == 2, "flush persists only retained messages", kTestName);
        insoulforge::MessageList restored(100);
        check(
          restored.snapshot() == fourth->messageSnapshot, "startup restoration rebuilds retained snapshot", kTestName);

        config.contextWindowLimit = originalContextLimit;
        config.memorySummaryTriggerCount = originalTriggerCount;
        config.memorySummaryBatchSize = originalBatchSize;
        config.memorySummaryContextCount = originalSummaryContextCount;
        database.close();
    }

} // namespace

auto main() -> int {
    testNewWorkflowNormalizesOneBotEvent();
    testNewWorkflowNormalizesOneBotNotices();
    testNewWorkflowDetectsCommands();
    testMessageRecordSemanticQueries();
    testNewWorkflowRouterHardRules();
    testJevUnconfiguredFallsBackToExistingBehavior();
    testRouteWithMockJev();
    testClassifyJevChoice();
    testJevClientChoiceAndConfidenceParsing();
    testRouterWindowStartIndexIsBatchedNotSliding();
    testMessageRecordProjectionHidesImageSources();
    testAssistantStickerRecordKeepsOnlyName();
    testSchemaMigration();
    testMemoryMaintenanceStore();
    testConversationMaintenanceStore();
    testImageDescriptionCacheStore();
    testUsageSummaryUsesLatestRoleModel();
    testMessageListSnapshotsAndPersistence();
    if (failures == 0) {
        std::cout << "All message contract tests passed\n";
        return 0;
    }
    std::cerr << failures << " contract test(s) failed\n";
    return 1;
}
