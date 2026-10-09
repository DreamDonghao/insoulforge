/// @file MessageContractTests.cpp
/// @brief 消息链路的契约测试

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/utils/coroutine.h>
#include <openssl/evp.h>

#include <admin/access/AdminAccessToken.hpp>
#include <admin/http/AdminController.hpp>
#include <agent/ability/AsyncTaskManager.hpp>
#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <agent/runtime/ExecutorAgent.hpp>
#include <agent/tools/ToolRegistry.hpp>
#include <agent/tools/custom/LuaToolExecutor.hpp>
#include <agent/tools/plugins/ActionToolsPlugin.hpp>
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
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <llm/JevClient.hpp>
#include <llm/usage/UsageStore.hpp>
#include <media/CharacterImageStore.hpp>
#include <media/ImageDescriptionService.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <media/ImageGenerationService.hpp>

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

        const auto systemEvent = insoulforge::OneBotEventNormalizer::normalize({{"post_type", "message"},
          {"message_type", "group"}, {"group_id", 100}, {"self_id", 42}, {"message_id", 1'790'000'000'000'001LL},
          {"sender", {{"user_id", insoulforge::SessionId::kSystemAccountId}, {"nickname", "系统异步任务"}}},
          {"message", insoulforge::json::array({{{"type", "text"}, {"data", {{"text", "任务完成"}}}}})}});
        check(systemEvent && insoulforge::MessageRecord::isSystem(*systemEvent),
          "async task event normalizes as a system message", kTestName);
        if (systemEvent) {
            const auto systemSnapshot = insoulforge::json::array({*systemEvent});
            const auto systemDecision =
              drogon::sync_wait(insoulforge::MessageRouter::route(100, "1790000000000001", systemSnapshot));
            check(systemDecision.shouldReply && systemDecision.isPriority,
              "async task result triggers a priority reply", kTestName);
        }
        const auto privateSystemEvent = insoulforge::OneBotEventNormalizer::normalize({{"post_type", "message"},
          {"message_type", "private"}, {"user_id", 11}, {"self_id", 42}, {"message_id", 1'790'000'000'000'002LL},
          {"sender", {{"user_id", insoulforge::SessionId::kSystemAccountId}, {"nickname", "系统异步任务"}}},
          {"message", insoulforge::json::array({{{"type", "text"}, {"data", {{"text", "任务失败"}}}}})}});
        check(privateSystemEvent && (*privateSystemEvent)["session_id"] == insoulforge::SessionId::fromPrivateUser(11),
          "async task result reaches the original private session", kTestName);

        config.selfQQNumber = originalSelfId;
    }

    void testJevUnconfiguredFallsBackToExistingBehavior() {
        constexpr std::string_view kTestName = "jev unconfigured falls back to existing behavior";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        auto &config = insoulforge::Config::instance();
        const auto originalJev = config.jev;
        const auto originalRouter = config.router;
        config.jev = {};
        config.router = {};

        const insoulforge::json plainSnapshot =
          insoulforge::json::array({{{"message_id", "plain"}, {"sender", {{"qq", "11"}}},
            {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "今天天气的大家都在干什么"}}})}}});
        const auto fallback = drogon::sync_wait(insoulforge::MessageRouter::route(100, "plain", plainSnapshot));
        check(fallback.shouldReply, "unconfigured jev falls back to existing fail-open path", kTestName);
        check(fallback.reason == "Router 请求失败", "fallback reason is unchanged", kTestName);

        config.jev = originalJev;
        config.router = originalRouter;
        database.close();
    }

    void testClassifyJevChoice() {
        constexpr std::string_view kTestName = "classify jev choice";
        using Action = insoulforge::RouterDecision::Action;

        const auto skip = insoulforge::MessageRouter::classifyJevChoice("skip", 0.9, 0.6);
        check(skip.has_value() && *skip == Action::SKIP, "skip label maps to SKIP", kTestName);
        const auto reply = insoulforge::MessageRouter::classifyJevChoice("reply", 0.9, 0.6);
        check(reply.has_value() && *reply == Action::REPLY, "reply label maps to REPLY", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("skip", 0.59, 0.6).has_value(),
          "low-confidence skip falls back", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("reply", 0.59, 0.6).has_value(),
          "low-confidence reply falls back", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("skip", std::nullopt, 0.6).has_value(),
          "missing confidence falls back", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("reply", std::numeric_limits<double>::quiet_NaN(), 0.6)
                .has_value(),
          "non-finite confidence falls back", kTestName);
        check(insoulforge::MessageRouter::classifyJevChoice("reply", 0.6, 0.6) == Action::REPLY,
          "threshold confidence is accepted", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("skip", 0.7, 0.8).has_value(),
          "configured threshold controls fallback", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("unclear", 0.9, 0.6).has_value(),
          "unclear label falls back to nullopt", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("other", 0.9, 0.6).has_value(),
          "unknown label falls back to nullopt", kTestName);
        check(!insoulforge::MessageRouter::classifyJevChoice("", 0.9, 0.6).has_value(),
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
        check(!insoulforge::JevClient::readChoice(
                insoulforge::json{{"answers", insoulforge::json::object()}}, "action", validLabels)
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
        check(insoulforge::MessageRouter::windowStartIndex(30) == 20, "window resets to keep at period boundary",
          kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(35) == 20, "window grows within a period", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(39) == 20, "window keeps a stable prefix within a period",
          kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(40) == 30, "next period starts a new prefix", kTestName);
        check(insoulforge::MessageRouter::windowStartIndex(5) == 0, "short snapshot starts at zero", kTestName);

        config.routerWindowTriggerCount = originalTrigger;
        config.routerWindowKeepCount = originalKeep;
    }

    void testMessageRecordProjectionHidesImageSources() {
        constexpr std::string_view kTestName = "message record projection";
        const insoulforge::json record = {
          {"time", "2026-09-07 17:22:05"},
          {"sender", {{"name", "Alice"}, {"qq", "11"}}},
          {"message_id", "7"},
          {"reply_to", "6"},
          {"segments", {{{"type", "text"}, {"text", "看这张图"}}, {{"type", "image"}, {"image_index", 0}}}},
          {"assets", {{"images", {{{"recognition_status", "succeeded"}, {"description", "一只蓝色的猫"},
                                   {"source", {{"file", "cat.jpg"}, {"url", "https://example.com/cat.jpg"}}}}}}}},
          {"memories", {{{"id", 1}, {"content", "用户偏好简洁回答"}, {"similarity", 0.8}}}},
        };

        const insoulforge::json projected = insoulforge::MessageRecord::projectForAgent(record);
        check(!projected.contains("assets"), "agent projection excludes assets", kTestName);
        check(projected["reply_to"] == "6", "agent projection retains quoted message ID", kTestName);
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

    void testGeneratedImageRecordKeepsDescription() {
        constexpr std::string_view kTestName = "generated image record";
        const auto record =
          insoulforge::MessageRecord::createAssistantGeneratedImageRecord("Bot(我)", 8, "画面中有一只白猫坐在窗边。");
        const auto projected = insoulforge::MessageRecord::projectForAgent(record);
        check(record["segments"].size() == 1 && record["segments"][0]["type"] == "image",
          "sent image remains an image segment", kTestName);
        check(projected["segments"][0]["description"] == "画面中有一只白猫坐在窗边。",
          "model sees the visual description", kTestName);
        check(insoulforge::MessageRecord::extractRecallText(record).find("白猫") != std::string::npos,
          "visual description is available to memory recall", kTestName);
        check(!record.dump().contains("base64://"), "record excludes image transport data", kTestName);
    }

    void testSchemaMigration() {
        constexpr std::string_view kTestName = "schema migration";
        sqlite3 *db = nullptr;
        check(sqlite3_open(":memory:", &db) == SQLITE_OK, "opens in-memory database", kTestName);
        if (!db)
            return;

        sqlite3_exec(db, "PRAGMA user_version = 6", nullptr, nullptr, nullptr);
        sqlite3_exec(db,
          "CREATE TABLE custom_tools (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, description TEXT NOT NULL, "
          "parameters TEXT, executor_type TEXT NOT NULL CHECK(executor_type IN ('python', 'http')), "
          "executor_config TEXT, script_content TEXT, readme TEXT, enabled INTEGER DEFAULT 1, "
          "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
          nullptr, nullptr, nullptr);
        sqlite3_exec(db,
          "INSERT INTO custom_tools (id, name, description, executor_type) VALUES (7, 'old_tool', 'old', 'python')",
          nullptr, nullptr, nullptr);
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
        const i32 oldToolResult = sqlite3_prepare_v2(
          db, "SELECT id, executor_type FROM custom_tools WHERE name='old_tool'", -1, &stmt, nullptr);
        check(oldToolResult == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 7,
          "migration preserves Python tool and ID", kTestName);
        sqlite3_finalize(stmt);
        check(sqlite3_exec(db,
                "INSERT INTO custom_tools (name, description, executor_type) VALUES ('new_tool', 'new', 'lua')",
                nullptr, nullptr, nullptr) == SQLITE_OK,
          "migration permits Lua tool", kTestName);
        sqlite3_close(db);
    }

    void testLuaToolExecutor() {
        constexpr std::string_view kTestName = "Lua custom tool";
        constexpr std::string_view script = R"lua(
            function run(args, ctx)
                local sent = bot.send_message(args.text)
                local task = bot.start_task("测试后台任务", {text = args.text})
                return ctx.session_id .. ":" .. sent.message_id .. ":" .. task.status
            end
            function background(payload, ctx)
                return payload.text .. ":" .. ctx.session_id
            end
        )lua";
        check(!insoulforge::LuaToolExecutor::validate(script), "valid script accepted", kTestName);
        check(insoulforge::LuaToolExecutor::validate("function other() end").has_value(), "missing run rejected",
          kTestName);
        const std::string result = drogon::sync_wait(
          insoulforge::LuaToolExecutor::execute(std::string(script), {{"text", "你好"}}, 123, false, true));
        check(result == "123:test-message:started", "host operations resume script", kTestName);
        const std::string background = drogon::sync_wait(
          insoulforge::LuaToolExecutor::execute(std::string(script), {{"text", "完成"}}, 123, true, true));
        check(background == "完成:123", "background entry receives payload", kTestName);
        try {
            std::ignore = drogon::sync_wait(insoulforge::LuaToolExecutor::execute(
              "function run() while true do end end", insoulforge::json::object(), 0, false, true));
            check(false, "infinite loop rejected", kTestName);
        } catch (const std::exception &) {
            check(true, "infinite loop rejected", kTestName);
        }
    }

    void testToolRegistryReload() {
        constexpr std::string_view kTestName = "tool registry reload";
        auto &registry = insoulforge::ToolRegistry::instance();
        const auto registerVersion = [&registry](std::string pluginId, std::string result) {
            return registry.registerPlugin(std::move(pluginId), [&result](insoulforge::ToolRegistry &staged) {
                staged.registerTool(
                  {.name = "contract_reload_tool",
                    .description = "test",
                    .handler = [result](insoulforge::json, insoulforge::ToolCallContext) -> drogon::Task<std::string> {
                        co_return result;
                    }},
                  insoulforge::ToolCategory::INFORMATION);
            });
        };
        check(registerVersion("contract.plugin", "old"), "initial registration succeeds", kTestName);
        check(!registerVersion("other.plugin", "conflict"), "cross-plugin name collision fails", kTestName);
        check(drogon::sync_wait(registry.executeTool("contract_reload_tool", {}, {})) == "old",
          "failed registration preserves old handler", kTestName);
        check(registerVersion("contract.plugin", "new"), "reload succeeds", kTestName);
        check(drogon::sync_wait(registry.executeTool("contract_reload_tool", {}, {})) == "new",
          "reload replaces handler", kTestName);
        registry.unregisterPlugin("contract.plugin");
    }

    void testFinalRoundToolPolicy() {
        constexpr std::string_view kTestName = "final round tool policy";
        auto &registry = insoulforge::ToolRegistry::instance();
        i32 executed = 0;
        const auto handler = [&executed](insoulforge::json, insoulforge::ToolCallContext) -> drogon::Task<std::string> {
            ++executed;
            co_return "executed";
        };
        check(registry.registerPlugin("contract.round-policy",
                [&](insoulforge::ToolRegistry &staged) {
                    staged.registerTool(
                      {.name = "contract_round_reply", .handler = handler}, insoulforge::ToolCategory::REPLY);
                    staged.registerTool({.name = "contract_round_group_reply",
                                          .handler = handler,
                                          .scope = insoulforge::ToolScope::GROUP_ONLY},
                      insoulforge::ToolCategory::REPLY);
                    staged.registerTool(
                      {.name = "contract_round_query", .handler = handler}, insoulforge::ToolCategory::INFORMATION);
                    staged.registerTool(
                      {.name = "contract_round_action", .handler = handler}, insoulforge::ToolCategory::ACTION);
                }),
          "policy tools register", kTestName);

        const auto names = [](const insoulforge::json &tools) {
            std::vector<std::string> result;
            for (const auto &tool: tools) {
                result.push_back(tool["function"]["name"].get<std::string>());
            }
            return result;
        };
        const auto regularNames = names(registry.getTools({}));
        check(std::ranges::find(regularNames, "contract_round_query") != regularNames.end() &&
                std::ranges::find(regularNames, "contract_round_action") != regularNames.end(),
          "normal rounds keep query and action tools", kTestName);
        const auto finalNames = names(registry.getTools({.replyOnly = true}));
        check(std::ranges::find(finalNames, "contract_round_reply") != finalNames.end(),
          "final round exposes reply tools", kTestName);
        check(std::ranges::find(finalNames, "contract_round_query") == finalNames.end() &&
                std::ranges::find(finalNames, "contract_round_action") == finalNames.end(),
          "final round excludes query and action tools", kTestName);
        const auto privateNames = names(registry.getTools({.isPrivateSession = true, .replyOnly = true}));
        check(std::ranges::find(privateNames, "contract_round_group_reply") == privateNames.end(),
          "reply filtering preserves private-session restrictions", kTestName);

        for (const auto name: {"contract_round_query", "contract_round_action"}) {
            const auto result = drogon::sync_wait(registry.executeTool(name, {}, {.replyOnly = true}));
            check(result.find("未执行") != std::string::npos && executed == 0,
              "non-reply calls cannot execute on final round", kTestName);
        }
        check(drogon::sync_wait(registry.executeTool("contract_round_reply", {}, {.replyOnly = true})) == "executed" &&
                executed == 1,
          "final-round policy allows reply category", kTestName);
        check(drogon::sync_wait(registry.executeTool("contract_round_query", {}, {})) == "executed" && executed == 2,
          "normal-round execution is unchanged", kTestName);
        registry.unregisterPlugin("contract.round-policy");
    }

    void testIterationRequestMessages() {
        constexpr std::string_view kTestName = "iteration request messages";
        const auto history = insoulforge::json::array(
          {{{"role", "system"}, {"content", "original prompt"}}, {{"role", "user"}, {"content", "question"}},
            {{"role", "assistant"}, {"tool_calls", {{{"id", "call-1"}, {"type", "function"}}}}},
            {{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "source data"}}});

        const auto first = insoulforge::ExecutorAgent::buildIterationMessages(history, 1, 8);
        const auto middle = insoulforge::ExecutorAgent::buildIterationMessages(history, 3, 8);
        const auto last = insoulforge::ExecutorAgent::buildIterationMessages(history, 8, 8);
        check(first.size() == history.size() + 1 && middle.size() == first.size() && last.size() == first.size(),
          "each request has exactly one transient execution status", kTestName);
        for (size_t index = 0; index < history.size(); ++index) {
            check(first[index] == history[index] && middle[index] == history[index] && last[index] == history[index],
              "request status preserves the full history and tool results", kTestName);
        }
        check(first.back()["role"] == "system" &&
                first.back()["content"].get<std::string>().find("还可请求模型 7 次") != std::string::npos,
          "first round counts future requests excluding the current one", kTestName);
        check(middle.back()["content"].get<std::string>().find("3/8") != std::string::npos &&
                middle.back()["content"].get<std::string>().find("还可请求模型 5 次") != std::string::npos,
          "intermediate status reflects the current round", kTestName);
        check(last.back()["content"].get<std::string>().find("还可请求模型 0 次") != std::string::npos &&
                last.back()["content"].get<std::string>().find("仅开放回复类工具") != std::string::npos,
          "last round explicitly requires a reply decision", kTestName);
        const auto single = insoulforge::ExecutorAgent::buildIterationMessages(history, 1, 1);
        check(single.back()["content"].get<std::string>().find("仅开放回复类工具") != std::string::npos,
          "a one-round limit is immediately reply-only", kTestName);
        check(history.size() == 4 && history.front()["content"] == "original prompt",
          "transient status never modifies the original conversation", kTestName);
    }

    void testToolRegistryReturnsHandlerErrors() {
        constexpr std::string_view kTestName = "tool handler error result";
        auto &registry = insoulforge::ToolRegistry::instance();
        check(registry.registerPlugin("contract.error",
                [](insoulforge::ToolRegistry &staged) {
                    staged.registerTool(
                      {.name = "contract_error_tool",
                        .description = "test",
                        .handler = [](insoulforge::json, insoulforge::ToolCallContext) -> drogon::Task<std::string> {
                            throw std::runtime_error("具体失败原因");
                            co_return "";
                        }},
                      insoulforge::ToolCategory::INFORMATION);
                }),
          "error tool registers", kTestName);
        const auto result = drogon::sync_wait(registry.executeTool("contract_error_tool", {}, {.sessionId = 100}));
        check(result.find("具体失败原因") != std::string::npos, "handler error is returned to model", kTestName);
        registry.unregisterPlugin("contract.error");
    }

    void testCharacterImageStore() {
        constexpr std::string_view kTestName = "character image storage";
        constexpr std::string_view encoded =
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9d1QAAAABJRU5ErkJggg==";
        std::string image(encoded.size() / 4 * 3, '\0');
        const i32 decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(image.data()),
          reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<i32>(encoded.size()));
        check(decoded > 0, "test PNG decodes", kTestName);
        image.resize(static_cast<size_t>(decoded) - 2);

        check(insoulforge::ImageDescriptionService::staticImageMimeType(image) == "image/png",
          "valid PNG is recognized", kTestName);
        check(insoulforge::ImageDescriptionService::staticImageDataUrl(image).value_or("").starts_with(
                "data:image/png;base64,"),
          "data URL retains MIME type", kTestName);
        check(!insoulforge::ImageDescriptionService::staticImageMimeType(image.substr(0, image.size() - 12)),
          "truncated PNG is rejected", kTestName);
        check(!insoulforge::ImageDescriptionService::staticImageMimeType("GIF89a"), "GIF is rejected", kTestName);

        const auto originalPath = std::filesystem::current_path();
        const auto testPath =
          std::filesystem::temp_directory_path() /
          ("insoulforge-character-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testPath);
        std::filesystem::current_path(testPath);
        check(!insoulforge::CharacterImageStore::load(), "initially empty", kTestName);
        check(!insoulforge::CharacterImageStore::save(image), "upload succeeds", kTestName);
        const auto stored = insoulforge::CharacterImageStore::load();
        check(stored && stored->bytes == image && stored->mimeType == "image/png", "saved bytes persist", kTestName);
        check(insoulforge::CharacterImageStore::save("invalid").has_value(), "invalid replacement rejected", kTestName);
        check(insoulforge::CharacterImageStore::load() && insoulforge::CharacterImageStore::load()->bytes == image,
          "failed replacement preserves original", kTestName);
        check(
          insoulforge::CharacterImageStore::save(std::string(insoulforge::CharacterImageStore::kMaxImageBytes + 1, 'x'))
            .has_value(),
          "oversized image rejected", kTestName);
        check(insoulforge::CharacterImageStore::remove(), "delete succeeds", kTestName);
        check(!insoulforge::CharacterImageStore::load(), "deleted image is absent", kTestName);
        std::filesystem::current_path(originalPath);
        std::filesystem::remove_all(testPath);
    }

    void testHttpTraceImageRedaction() {
        constexpr std::string_view kTestName = "HTTP image trace redaction";
        const auto imageData = std::string("data:image/png;base64,") + std::string(2048, 'A');
        insoulforge::json request = {{"model", "image-model"}, {"prompt", "画出你自己"}, {"size", "1024x1024"}};
        request["input_references"] =
          insoulforge::json::array({{{"type", "image_url"}, {"image_url", {{"url", imageData}}}}});
        const auto redacted = insoulforge::HttpUtil::redactImagePayloads(request);
        check(redacted["prompt"] == "画出你自己" && redacted["size"] == "1024x1024",
          "prompt and dimensions remain visible", kTestName);
        check(redacted["input_references"][0]["image_url"]["url"] == "[图片数据已省略]", "only image URL is redacted",
          kTestName);
        check(
          request["input_references"][0]["image_url"]["url"] == imageData, "actual request stays unchanged", kTestName);

        const insoulforge::json response = {
          {"created", 123}, {"data", {{{"revised_prompt", "保留这段描述"}, {"b64_json", std::string(2048, 'A')}}}}};
        const auto redactedResponse = insoulforge::HttpUtil::redactImagePayloads(response);
        check(redactedResponse["data"][0]["revised_prompt"] == "保留这段描述", "response metadata remains visible",
          kTestName);
        check(redactedResponse["data"][0]["b64_json"] == "[图片数据已省略]", "response image is redacted", kTestName);
    }

    void testCharacterImageToolRequiresUpload() {
        constexpr std::string_view kTestName = "self image tool reference";
        auto &registry = insoulforge::ToolRegistry::instance();
        check(registry.registerPlugin("contract.character-image",
                [](insoulforge::ToolRegistry &staged) {
                    insoulforge::ActionToolsPlugin{"test-bot"}.registerTools(staged);
                }),
          "action tools register", kTestName);
        const auto oldConfig = insoulforge::Config::instance().imageGeneration;
        auto &api = insoulforge::Config::instance().imageGeneration;
        api.apiKey = "test";
        api.baseUrl = "https://example.com";
        api.path = "/images";
        api.model = "test";
        const auto result = drogon::sync_wait(registry.executeTool(
          "generate_image", {{"prompt", "画出你自己"}, {"use_self_reference", true}}, {.sessionId = 123}));
        check(result.find("尚未在管理后台上传") != std::string::npos, "missing image rejects before task starts",
          kTestName);
        api = oldConfig;
        registry.unregisterPlugin("contract.character-image");
    }

    void testMemoryMaintenanceStore() {
        constexpr std::string_view kTestName = "memory maintenance store";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);

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
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);

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

    void testMemoryModelConfigMigration() {
        constexpr std::string_view kTestName = "memory model config migration";
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-memory-config-test-" + std::to_string(suffix) + ".json");
        {
            std::ofstream output(path);
            output
              << R"({"llm":{"executor":{"apiKey":"old-key","baseUrl":"https://old.example.com/v1","path":"/chat/completions","model":"old-model","maxTokens":150,"temperature":0.7,"topP":0.9,"reasoningEffort":""}},"memory":{"memoryExtractMaxTokens":8192}})";
        }
        check(insoulforge::Config::instance().initialize(path.string()).has_value(), "initializes config", kTestName);
        const auto defaultGeneration = insoulforge::Config::instance().getLLMConfig("imageGeneration");
        check(defaultGeneration.value("baseUrl", "") == "https://api.openai.com/v1",
          "creates official image generation URL default", kTestName);
        check(defaultGeneration.value("model", "") == "gpt-image-2.5-flare",
          "creates official image generation model default", kTestName);
        const auto memory = insoulforge::Config::instance().getLLMConfig("memory");
        check(memory.value("model", "") == "old-model", "inherits executor model", kTestName);
        check(memory.value("maxTokens", 0) == 8192, "preserves old memory token limit", kTestName);
        check(memory.value("temperature", 0.0) == 0.4, "preserves extraction sampling temperature", kTestName);
        check(!insoulforge::Config::instance().getMemoryConfig().contains("memoryExtractMaxTokens"),
          "removes legacy token field", kTestName);
        std::filesystem::remove(path);
    }

    void testStartupResults() {
        constexpr std::string_view kTestName = "startup initialization results";
        check(insoulforge::AdminAccessToken::initialize().has_value(), "generates admin token", kTestName);
        check(insoulforge::AdminAccessToken::token().size() == 64, "token contains 32 random bytes encoded as hex",
          kTestName);

        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        const auto oldConnection = database.handle();
        const auto directory =
          std::filesystem::temp_directory_path() /
          ("insoulforge-db-init-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        const auto blocked = directory / "blocked";
        {
            std::ofstream output(blocked);
            output << "not a directory";
        }
        const auto directoryFailure = database.initialize((blocked / "db.sqlite").string());
        check(!directoryFailure && !directoryFailure.error().empty() && database.handle() == oldConnection,
          "directory failure returns a reason and preserves connection", kTestName);
        check(!database.initialize(directory.string()) && database.handle() == oldConnection,
          "database open failure preserves connection", kTestName);
        const auto invalidDatabase = directory / "invalid.sqlite";
        {
            std::ofstream output(invalidDatabase);
            output << "this is not a SQLite database";
        }
        check(!database.initialize(invalidDatabase.string()) && database.handle() == oldConnection,
          "migration failure returns an error and preserves connection", kTestName);

        auto &agent = insoulforge::AgentSystem::instance();
        check(agent.initialize(database, "explicit-bot-name").has_value() && agent.isReady(),
          "successful Agent initialization marks ready", kTestName);
        bool foundScheduleTool = false;
        for (const auto &tool: insoulforge::ToolRegistry::instance().getAllTools()) {
            const auto &function = tool["function"];
            if (function["name"] == "create_scheduled_task") {
                foundScheduleTool = true;
                check(function["parameters"]["properties"]["content"]["description"].get<std::string>().find(
                        "explicit-bot-name") != std::string::npos,
                  "tool registration uses the supplied name rather than global configuration", kTestName);
            }
        }
        check(foundScheduleTool, "schedule tool is registered", kTestName);
        check(sqlite3_exec(database.handle(), "DROP TABLE prompts", nullptr, nullptr, nullptr) == SQLITE_OK,
          "simulates unavailable prompt storage", kTestName);
        const auto agentFailure = agent.initialize(database, "explicit-bot-name");
        check(!agentFailure && !agentFailure.error().empty() && !agent.isReady(),
          "Agent dependency failure returns an error and clears ready state", kTestName);
        database.close();
        check(!agent.initialize(database, "explicit-bot-name") && !agent.isReady(),
          "uninitialized database is rejected before dependency access", kTestName);
        std::filesystem::remove_all(directory);
    }

    void testAdminRequestAccess() {
        constexpr std::string_view kTestName = "admin request access";
        using insoulforge::AdminAccessToken;
        check(AdminAccessToken::initialize().has_value(), "initializes token", kTestName);
        const auto loginResponse = drogon::HttpResponse::newHttpResponse();
        AdminAccessToken::grantSession(loginResponse);
        for (const auto *path: {"/index.html", "/onebot", "/admin/api/auth/login", "/admin/api/auth/status",
               "/admin/api/execution-config", "/admin/ws", "/admin/logs/ws", "/admin/api/auth/login/"}) {
            const std::string_view route = path;
            const bool protectedRoute =
              route.starts_with("/admin/api/") || route == "/admin/ws" || route == "/admin/logs/ws";
            const bool publicAuth = route == "/admin/api/auth/login" || route == "/admin/api/auth/status";
            for (const auto authenticated: {false, true}) {
                const auto request = drogon::HttpRequest::newHttpRequest();
                request->setPath(path);
                if (authenticated) {
                    for (const auto &[name, cookie]: loginResponse->cookies()) {
                        request->addCookie(name, cookie.value());
                    }
                }
                bool continued = false;
                drogon::HttpResponsePtr rejected;
                AdminAccessToken::checkRequestAccess(
                  request, [&rejected](const drogon::HttpResponsePtr &response) { rejected = response; },
                  [&continued] { continued = true; });
                const bool allowed = !protectedRoute || publicAuth || authenticated;
                check(continued == allowed && static_cast<bool>(rejected) == !allowed,
                  "calls exactly one routing callback", kTestName);
                if (!allowed && rejected) {
                    check(rejected->getStatusCode() == drogon::k401Unauthorized &&
                            insoulforge::parseJson(std::string(rejected->body()))["success"] == false,
                      "protected request returns the original unauthorized response", kTestName);
                }
            }
        }
    }

    void testConfigInitialization() {
        constexpr std::string_view kTestName = "explicit configuration initialization";
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory =
          std::filesystem::temp_directory_path() / ("insoulforge-config-init-" + std::to_string(suffix));
        std::filesystem::create_directory(directory);
        const auto path = directory / "selected.json";
        const auto temporaryPath = directory / "selected.json.tmp";
        std::filesystem::create_directory(temporaryPath);
        const auto failed = insoulforge::Config::instance().initialize(path.string());
        check(!failed && failed.error().type == insoulforge::ConfigErrorType::FileOperation,
          "first initialization returns file failure", kTestName);
        std::filesystem::remove(temporaryPath);
        check(insoulforge::Config::instance().initialize(path.string()).has_value() && std::filesystem::exists(path),
          "retry creates defaults at the explicitly selected path", kTestName);
        check(insoulforge::Config::instance().getExecutionConfig()["maxToolRounds"] == 8,
          "created defaults are readable", kTestName);
        std::filesystem::remove_all(directory);
    }

    void testConfigFacade() {
        constexpr std::string_view kTestName = "configuration facade";
        const auto directory =
          std::filesystem::temp_directory_path() /
          ("insoulforge-config-facade-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        const auto path = directory / "config.json";
        auto &config = insoulforge::Config::instance();
        check(config.initialize(path.string()).has_value(), "initializes storage and runtime together", kTestName);
        check(config.saveQQConfig({{"botName", "facade-bot"}, {"selfQQNumber", 3457355246ULL}}).has_value() &&
                config.botName == "facade-bot" && config.selfQQNumber == 3457355246ULL,
          "saving immediately applies typed QQ configuration", kTestName);
        const auto oldQQ = config.getQQConfig();
        for (const auto &invalid:
          insoulforge::json::array({{{"selfQQNumber", -1}}, {{"selfQQNumber", 123.5}}, {{"botName", 23}}})) {
            const auto result = config.saveQQConfig(invalid);
            check(!result && result.error().type == insoulforge::ConfigErrorType::InvalidArgument &&
                    config.getQQConfig() == oldQQ && config.botName == "facade-bot",
              "invalid conversion leaves storage and runtime unchanged", kTestName);
        }
        check(
          !config.saveLLMConfig("router", {{"maxTokens", 100.5}}), "fractional token limits are rejected", kTestName);
        check(!config.saveLLMConfig("router", {{"temperature", 3.0}}), "out-of-range sampling parameters are rejected",
          kTestName);
        const auto invalidPath = directory / "invalid.json";
        const std::string invalidContent =
          R"({"llm":{"router":{"maxTokens":4294967296}},"qq":{"botName":"not-applied"}})";
        {
            std::ofstream output(invalidPath);
            output << invalidContent;
        }
        const auto invalidInit = config.initialize(invalidPath.string());
        check(!invalidInit && invalidInit.error().type == insoulforge::ConfigErrorType::InvalidArgument &&
                config.botName == "facade-bot" && config.getQQConfig() == oldQQ,
          "failed startup conversion preserves existing runtime and storage", kTestName);
        {
            std::ifstream input(invalidPath);
            const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            check(content == invalidContent, "conversion failure does not overwrite the source file", kTestName);
        }
        config.executor.model = "unrelated-runtime-model";
        check(config.saveExecutionConfig({{"maxToolRounds", 16}}).has_value() &&
                config.execution.maxToolRounds.load() == 16,
          "saving after failed initialization still uses the previous file", kTestName);
        check(config.executor.model == "unrelated-runtime-model",
          "execution saves do not rewrite unrelated runtime fields", kTestName);
        check(config.initialize(path.string()).has_value() && config.execution.maxToolRounds.load() == 16 &&
                config.botName == "facade-bot",
          "unified initialization restores saved runtime values", kTestName);
        std::filesystem::remove_all(directory);
    }

    void testExecutionConfig() {
        constexpr std::string_view kTestName = "execution config";
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-execution-config-test-" + std::to_string(suffix) + ".json");
        {
            std::ofstream output(path);
            output << R"({"qq":{"botName":"existing-bot"}})";
        }
        check(insoulforge::Config::instance().initialize(path.string()).has_value(), "initializes config", kTestName);
        check(insoulforge::Config::instance().getExecutionConfig()["maxToolRounds"] == 8,
          "old files receive the default limit", kTestName);
        check(insoulforge::Config::instance().execution.maxToolRounds.load() == 8, "loads the default runtime limit",
          kTestName);
        check(insoulforge::Config::instance()
                .saveExecutionConfig({{"maxToolRounds", 8}, {"futureSetting", true}})
                .has_value(),
          "saves execution config", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto callback = [&response](const drogon::HttpResponsePtr &value) { response = value; };
        drogon::sync_wait(controller.getExecutionConfig(drogon::HttpRequest::newHttpRequest(), callback));
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::parseJson(std::string(response->body()))["maxToolRounds"] == 8,
          "GET returns stored execution settings", kTestName);

        const auto save = [&](const std::string &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(body);
            drogon::sync_wait(controller.saveExecutionConfig(request, callback));
        };
        for (const auto limit: {1, 32, 100}) {
            save(insoulforge::dumpJson({{"maxToolRounds", limit}}));
            check(response->getStatusCode() == drogon::k200OK &&
                    insoulforge::parseJson(std::string(response->body()))["success"] == true,
              "accepts valid limits including boundaries", kTestName);
            check(insoulforge::Config::instance().execution.maxToolRounds.load() == limit,
              "save updates runtime without reload", kTestName);
        }
        check(insoulforge::Config::instance().getExecutionConfig()["futureSetting"] == true,
          "saving preserves other execution fields", kTestName);
        check(insoulforge::Config::instance().getQQConfig()["botName"] == "existing-bot",
          "saving leaves other configuration sections unchanged", kTestName);

        const auto stored = insoulforge::Config::instance().getExecutionConfig();
        const auto invalidValues =
          insoulforge::json::array({0, -1, 101, 8.0, 8.5, "8", true, nullptr, std::numeric_limits<u64>::max()});
        for (const auto &value: invalidValues) {
            save(insoulforge::dumpJson({{"maxToolRounds", value}}));
            check(response->getStatusCode() == drogon::k400BadRequest,
              "POST rejects non-integers and out-of-range limits", kTestName);
            check(insoulforge::Config::instance().getExecutionConfig() == stored &&
                    insoulforge::Config::instance().execution.maxToolRounds.load() == 100,
              "invalid saves do not change stored or runtime values", kTestName);
        }
        for (const auto body: {"{}", "[]", "null", "not json"}) {
            save(body);
            check(response->getStatusCode() == drogon::k400BadRequest,
              "POST rejects missing settings and invalid JSON bodies", kTestName);
        }

        // Blocking the temporary path makes the write fail even when tests run with elevated permissions.
        const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
        std::filesystem::create_directory(temporaryPath);
        save(R"({"maxToolRounds":16})");
        check(
          response->getStatusCode() == drogon::k500InternalServerError, "file write errors are reported", kTestName);
        check(insoulforge::Config::instance().getExecutionConfig() == stored &&
                insoulforge::Config::instance().execution.maxToolRounds.load() == 100,
          "write failure leaves stored and runtime values unchanged", kTestName);
        const auto oldQQ = insoulforge::Config::instance().getQQConfig();
        const auto oldMemory = insoulforge::Config::instance().getMemoryConfig();
        const auto oldModels = insoulforge::Config::instance().getAllLLMConfigs();
        auto qqRequest = drogon::HttpRequest::newHttpRequest();
        qqRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        qqRequest->setBody(insoulforge::dumpJson({{"botName", "not-applied"}, {"selfQQNumber", 123},
          {"oneBotTransport", "http"}, {"qqHttpHost", "http://not-applied"}}));
        const auto oldBotName = insoulforge::Config::instance().botName;
        const auto oldTransport = insoulforge::Config::instance().oneBotTransport;
        drogon::sync_wait(controller.saveQQConfig(qqRequest, callback));
        check(response->getStatusCode() == drogon::k500InternalServerError &&
                insoulforge::Config::instance().botName == oldBotName &&
                insoulforge::Config::instance().oneBotTransport == oldTransport,
          "QQ HTTP write failure does not apply runtime settings", kTestName);
        check(!insoulforge::Config::instance().saveQQConfig({{"botName", "not-saved"}}) &&
                insoulforge::Config::instance().getQQConfig() == oldQQ,
          "QQ write failure preserves memory state", kTestName);
        check(!insoulforge::Config::instance().saveMemoryConfig({{"contextWindowLimit", 23}}) &&
                insoulforge::Config::instance().getMemoryConfig() == oldMemory,
          "memory write failure preserves memory state", kTestName);
        check(!insoulforge::Config::instance().saveLLMConfig("router", {{"model", "not-saved"}}) &&
                insoulforge::Config::instance().getAllLLMConfigs() == oldModels,
          "model write failure preserves memory state", kTestName);
        const auto invalidSave = insoulforge::Config::instance().saveMemoryConfig(nullptr);
        check(!invalidSave && invalidSave.error().type == insoulforge::ConfigErrorType::InvalidArgument,
          "invalid section returns a typed error", kTestName);

        const auto failedPath = std::filesystem::path(path.string() + ".new");
        const auto blockedPath = std::filesystem::path(failedPath.string() + ".tmp");
        std::filesystem::create_directory(blockedPath);
        const auto failedInit = insoulforge::Config::instance().initialize(failedPath.string());
        check(!failedInit && failedInit.error().type == insoulforge::ConfigErrorType::FileOperation &&
                insoulforge::Config::instance().getQQConfig() == oldQQ &&
                insoulforge::Config::instance().getExecutionConfig() == stored,
          "failed reinitialization preserves existing configuration", kTestName);
        std::filesystem::remove(blockedPath);
        std::filesystem::remove(temporaryPath);

        check(insoulforge::Config::instance().saveMemoryConfig(oldMemory).has_value(),
          "failed reinitialization preserves the original file path", kTestName);
        check(!std::filesystem::exists(failedPath), "failure does not create the requested config file", kTestName);

        check(insoulforge::Config::instance().initialize(path.string()).has_value(), "reinitializes config", kTestName);
        check(insoulforge::Config::instance().execution.maxToolRounds.load() == 100,
          "saved values survive reinitialization", kTestName);

        for (const auto &value: invalidValues) {
            {
                std::ofstream output(path);
                output << insoulforge::dumpJson({{"execution", {{"maxToolRounds", value}}}});
            }
            check(insoulforge::Config::instance().initialize(path.string()).has_value(), "repairs config", kTestName);
            check(insoulforge::Config::instance().getExecutionConfig()["maxToolRounds"] == 8,
              "startup repairs invalid file values", kTestName);
            std::ifstream input(path);
            const auto repaired = insoulforge::json::parse(input);
            check(repaired["execution"]["maxToolRounds"] == 8, "startup persists the repaired default", kTestName);
        }
        std::filesystem::remove(path);
    }

    void testToolHistoryRecords() {
        constexpr std::string_view kTestName = "tool history records";
        const insoulforge::json arguments{{"query", "测试搜索"}, {"apiKey", "private-key"},
          {"headers", {{"Authorization", "Bearer private-token"}}}, {"image", "data:image/png;base64,aGVsbG8="}};
        const auto search = insoulforge::MessageRecord::createToolHistoryEntry("search_web", arguments, "returned");
        check(search["arguments"]["query"] == "测试搜索", "keeps ordinary tool arguments", kTestName);
        check(insoulforge::dumpJson(search).find("private-") == std::string::npos &&
                insoulforge::dumpJson(search).find("aGVsbG8=") == std::string::npos,
          "history does not retain credentials or image bytes", kTestName);
        check(arguments["apiKey"] == "private-key", "sanitizing history does not alter execution arguments", kTestName);
        const auto large =
          insoulforge::MessageRecord::createToolHistoryEntry("custom", {{"value", std::string(3000, '.')}}, "returned");
        check(large["arguments"].contains("_omitted"), "oversized arguments are explicitly omitted", kTestName);

        const auto noReply =
          insoulforge::MessageRecord::createToolHistoryEntry("no_reply", insoulforge::json::object(), "decision");
        const auto history = insoulforge::json::array({search, noReply});
        const auto record =
          insoulforge::MessageRecord::createAssistantExecutionRecord("机器人(我)", history, "no_reply");
        check(insoulforge::MessageRecord::isAssistant(record) && !record.contains("message_id") &&
                record["segments"].empty() && record["execution_status"] == "no_reply",
          "no_reply is an internal assistant record, not a sent QQ message", kTestName);
        check(record["tool_history"][0]["name"] == "search_web" && record["tool_history"][1]["name"] == "no_reply",
          "history keeps call order and the no_reply decision", kTestName);
        check(!insoulforge::MessageRecord::projectForAgent(record).contains("tool_history"),
          "router and maintenance projections omit tool history", kTestName);
        check(insoulforge::MessageRecord::projectForAgent(record, true)["tool_history"] == history,
          "executor projection retains internal history", kTestName);

        auto sent = insoulforge::MessageRecord::createAssistantRecord("机器人(我)", 123, "最终回复");
        sent["tool_history"] = history;
        check(insoulforge::MessageRecord::projectForAgent(sent, true)["tool_history"] == history &&
                insoulforge::MessageRecord::extractText(sent) == "最终回复",
          "sent messages carry history without changing outgoing text", kTestName);
        sent["sender"]["qq"] = "11";
        check(!insoulforge::MessageRecord::projectForAgent(sent, true).contains("tool_history"),
          "user records cannot claim assistant tool history", kTestName);

        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        insoulforge::MessageList messages(987654, database, insoulforge::Config::instance());
        const auto update = messages.append(record);
        check(update && update->messageSnapshot.back()["tool_history"] == history,
          "no_reply history enters the context snapshot", kTestName);
        messages.flushToStorage();
        insoulforge::MessageList restored(987654, database, insoulforge::Config::instance());
        check(restored.fullSnapshot() == messages.fullSnapshot(), "no_reply survives persistence and restoration",
          kTestName);
        database.close();
    }

    void testImageDescriptionCacheStore() {
        constexpr std::string_view kTestName = "image description cache store";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        check(insoulforge::Config::instance().initialize("data/message-contract-test-config.json").has_value(),
          "initializes config", kTestName);

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

        check(insoulforge::Config::instance()
                .saveLLMConfig(
                  "image", {{"apiKey", "key"}, {"baseUrl", "https://example.com"}, {"path", "/v1/chat/completions"},
                             {"model", "vision-model"}, {"maxTokens", 1536}, {"temperature", 0.4}, {"topP", 0.8},
                             {"reasoningEffort", ""}, {"minConfidence", 0.2}})
                .has_value(),
          "saves image config", kTestName);
        check(!insoulforge::Config::instance().getLLMConfig("image").contains("minConfidence"),
          "Jev threshold is not stored with other models", kTestName);
        check(insoulforge::Config::instance().imageParams.maxTokens == 1536, "loads configured image max tokens",
          kTestName);

        check(insoulforge::Config::instance()
                .saveLLMConfig(
                  "imageGeneration", {{"apiKey", "image-key"}, {"baseUrl", "https://example.com/v1"},
                                       {"path", "/images/generations"}, {"model", "test-image"}, {"maxTokens", 100}})
                .has_value(),
          "saves image generation config", kTestName);
        check(!insoulforge::Config::instance().getLLMConfig("imageGeneration").contains("maxTokens"),
          "image generation does not persist chat parameters", kTestName);
        check(insoulforge::Config::instance().imageGeneration.model == "test-image", "loads image generation model",
          kTestName);

        check(
          insoulforge::Config::instance()
            .saveLLMConfig("memory", {{"apiKey", "memory-key"}, {"baseUrl", "https://memory.example.com/v1"},
                                       {"path", "/chat/completions"}, {"model", "memory-model"}, {"maxTokens", 2048},
                                       {"temperature", 0.4}, {"topP", 0.9}, {"reasoningEffort", "none"}})
            .has_value(),
          "saves memory model config", kTestName);
        check(
          insoulforge::Config::instance().memory.model == "memory-model", "loads independent memory model", kTestName);
        check(insoulforge::Config::instance().memoryParams.maxTokens == 2048, "loads independent memory token limit",
          kTestName);
        check(
          insoulforge::Config::instance().memory.reasoningEffort == "none", "loads memory reasoning effort", kTestName);

        check(insoulforge::Config::instance()
                .saveLLMConfig("jev", {{"apiKey", "key"}, {"baseUrl", "https://example.com"}, {"path", "/decisions"},
                                        {"model", "jev-model"}, {"minConfidence", 0.8}})
                .has_value(),
          "saves Jev config", kTestName);
        check(insoulforge::Config::instance().jevMinConfidence == 0.8, "loads configured Jev confidence threshold",
          kTestName);
        check(insoulforge::Config::instance()
                .saveLLMConfig("jev", {{"apiKey", "key"}, {"baseUrl", "https://example.com"}, {"path", "/decisions"},
                                        {"model", "jev-model"}})
                .has_value(),
          "preserves Jev threshold", kTestName);
        check(insoulforge::Config::instance().getLLMConfig("jev")["minConfidence"] == 0.8,
          "saving Jev without a threshold keeps the previous value", kTestName);
        database.close();
    }

    void testImageGenerationResponse() {
        constexpr std::string_view kTestName = "image generation response";
        const auto urlImage = insoulforge::ImageGenerationService::imageCode(
          {{"data", {{{"url", "https://example.com/image.png?a=1&b=2"}}}}});
        check(urlImage && urlImage->message == "[CQ:image,file=https://example.com/image.png?a=1&amp;b=2]" &&
                urlImage->source == "https://example.com/image.png?a=1&b=2",
          "escapes URL for OneBot CQ", kTestName);
        const auto dataUrlImage =
          insoulforge::ImageGenerationService::imageCode({{"data", {{{"url", "data:image/png;base64,aGVsbG8="}}}}});
        check(dataUrlImage && dataUrlImage->message == "[CQ:image,file=base64://aGVsbG8=]" &&
                dataUrlImage->source == "base64://aGVsbG8=",
          "accepts base64 data URL", kTestName);
        const auto rawUrlImage = insoulforge::ImageGenerationService::imageCode({{"data", {{{"url", "aGVsbG8="}}}}});
        check(rawUrlImage && rawUrlImage->message == "[CQ:image,file=base64://aGVsbG8=]",
          "accepts raw base64 URL field", kTestName);
        const auto base64Image =
          insoulforge::ImageGenerationService::imageCode({{"data", {{{"b64_json", "aGVsbG8="}}}}});
        check(base64Image && base64Image->message == "[CQ:image,file=base64://aGVsbG8=]", "accepts base64 image",
          kTestName);
        check(!insoulforge::ImageGenerationService::imageCode({{"data", {{{"b64_json", "bad!"}}}}}),
          "rejects malformed base64", kTestName);
        check(!insoulforge::ImageGenerationService::imageCode({{"data", insoulforge::json::array()}}),
          "rejects missing images", kTestName);
    }

    void testUsageSummaryUsesLatestRoleModel() {
        constexpr std::string_view kTestName = "usage summary model selection";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);

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
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);

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

        insoulforge::MessageList messages(100, database, config);
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
        insoulforge::MessageList restored(100, database, config);
        check(
          restored.snapshot() == fourth->messageSnapshot, "startup restoration rebuilds retained snapshot", kTestName);

        config.contextWindowLimit = originalContextLimit;
        config.memorySummaryTriggerCount = originalTriggerCount;
        config.memorySummaryBatchSize = originalBatchSize;
        config.memorySummaryContextCount = originalSummaryContextCount;
        database.close();
    }

    void testAsyncTaskSessionExclusivity() {
        constexpr std::string_view kTestName = "async task session exclusivity";
        auto &database = insoulforge::Database::instance();
        auto &config = insoulforge::Config::instance();
        auto &agent = insoulforge::AgentSystem::instance();
        auto &workflow = insoulforge::OneBotEventWorkflow::instance();
        database.close();
        check(!workflow.getSessionMessages(555), "getting the workflow does not restore sessions", kTestName);
        check(!workflow.initialize(database, config, agent), "uninitialized database returns an error", kTestName);
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        check(insoulforge::Config::instance().initialize("data/message-contract-test-config.json").has_value(),
          "initializes config", kTestName);


        check(agent.initialize(database, config.botName).has_value(), "initializes Agent", kTestName);
        check(sqlite3_exec(database.handle(), "DROP TABLE chat_records", nullptr, nullptr, nullptr) == SQLITE_OK,
          "simulates restore failure", kTestName);
        const auto failedRestore = workflow.initialize(database, config, agent);
        check(!failedRestore && !failedRestore.error().empty() && !workflow.getSessionMessages(555),
          "restore failure returns an error without publishing sessions", kTestName);
        check(database.initialize(":memory:").has_value(), "recreates database after failure", kTestName);
        check(agent.initialize(database, config.botName).has_value(), "reinitializes Agent dependencies", kTestName);
        const auto restoredMessage = insoulforge::MessageRecord::createAssistantRecord("test", 777, "restored");
        insoulforge::ChatRecordStore::addChatRecord(database, 555, "assistant", insoulforge::dumpJson(restoredMessage));
        check(workflow.initialize(database, config, agent).has_value(), "initializes workflow", kTestName);
        const auto restored = workflow.getSessionMessages(555);
        check(restored && restored->size() == 1 && restored->front() == restoredMessage,
          "explicit initialization restores persisted messages", kTestName);
        workflow.appendDeliveredAssistantMessage(
          555, insoulforge::MessageRecord::createAssistantRecord("test", 778, "new message"));
        check(workflow.initialize(database, config, agent).has_value() && workflow.getSessionMessages(555)->size() == 2,
          "repeat initialization does not overwrite live messages", kTestName);
        auto &tasks = insoulforge::AsyncTaskManager::instance();
        const auto delayedResult = []() -> drogon::Task<insoulforge::AsyncTaskManager::Result> {
            co_await drogon::sleepCoro(drogon::app().getLoop(), 60.0);
            co_return insoulforge::AsyncTaskManager::Result{.content = "unused"};
        };
        const auto first = tasks.start(100, "测试任务", delayedResult);
        const auto busy = tasks.start(100, "重复任务", delayedResult);
        const auto otherSession = tasks.start(200, "独立任务", delayedResult);
        check(
          first.status == insoulforge::AsyncTaskManager::StartResult::Status::Started, "first task starts", kTestName);
        check(busy.status == insoulforge::AsyncTaskManager::StartResult::Status::Busy && busy.taskId == first.taskId,
          "same session returns active task", kTestName);
        check(otherSession.status == insoulforge::AsyncTaskManager::StartResult::Status::Started,
          "different session starts independently", kTestName);

        const auto messages = insoulforge::OneBotEventWorkflow::instance().getSessionMessages(100);
        check(messages && messages->size() == 1, "start writes one status record", kTestName);
        if (messages && !messages->empty()) {
            check(insoulforge::MessageRecord::isSystem((*messages)[0]), "status has system sender", kTestName);
            check(insoulforge::MessageRecord::projectForAgent((*messages)[0])["segments"][0]["text"]
                      .get<std::string>()
                      .find("已开始") != std::string::npos,
              "status is visible in model projection", kTestName);
        }

        tasks.stop();
        const auto stopped = tasks.start(300, "退出后任务", delayedResult);
        check(stopped.status == insoulforge::AsyncTaskManager::StartResult::Status::Stopping,
          "shutdown rejects new tasks", kTestName);
        const auto stoppedMessages = insoulforge::OneBotEventWorkflow::instance().getSessionMessages(100);
        check(stoppedMessages && stoppedMessages->size() == 2 &&
                insoulforge::MessageRecord::extractText(stoppedMessages->back()).find("中断") != std::string::npos,
          "shutdown records interruption", kTestName);
        database.close();
    }
} // namespace

auto main() -> int {
    testAdminRequestAccess();
    testConfigInitialization();
    testNewWorkflowNormalizesOneBotEvent();
    testNewWorkflowNormalizesOneBotNotices();
    testNewWorkflowDetectsCommands();
    testMessageRecordSemanticQueries();
    testNewWorkflowRouterHardRules();
    testJevUnconfiguredFallsBackToExistingBehavior();
    testClassifyJevChoice();
    testJevClientChoiceAndConfidenceParsing();
    testRouterWindowStartIndexIsBatchedNotSliding();
    testMessageRecordProjectionHidesImageSources();
    testAssistantStickerRecordKeepsOnlyName();
    testGeneratedImageRecordKeepsDescription();
    testSchemaMigration();
    testLuaToolExecutor();
    testToolRegistryReload();
    testFinalRoundToolPolicy();
    testIterationRequestMessages();
    testToolRegistryReturnsHandlerErrors();
    testCharacterImageStore();
    testHttpTraceImageRedaction();
    testCharacterImageToolRequiresUpload();
    testMemoryMaintenanceStore();
    testConversationMaintenanceStore();
    testMemoryModelConfigMigration();
    testConfigFacade();
    testExecutionConfig();
    testToolHistoryRecords();
    testImageDescriptionCacheStore();
    testImageGenerationResponse();
    testUsageSummaryUsesLatestRoleModel();
    testMessageListSnapshotsAndPersistence();
    testAsyncTaskSessionExclusivity();
    testStartupResults();
    if (failures == 0) {
        std::cout << "All message contract tests passed\n";
        return 0;
    }
    std::cerr << failures << " contract test(s) failed\n";
    return 1;
}
