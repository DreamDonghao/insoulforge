/// @file MessageContractTests.cpp
/// @brief 消息链路的契约测试

#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/maintenance/ConversationMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>
#include <conversation/maintenance/affinity/AffinityStore.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/CommandProcessor.hpp>
#include <conversation/workflow/MessageList.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <conversation/workflow/OneBotEventNormalizer.hpp>
#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <exception>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <iostream>
#include <llm/UsageStore.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    int failures = 0;

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
        const uint64_t originalSelfId = config.selfQQNumber;
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
        const uint64_t originalSelfId = config.selfQQNumber;
        config.selfQQNumber = 42;

        const insoulforge::json mentionSnapshot = insoulforge::json::array({{{"sender", {{"qq", "11"}}},
          {"segments", insoulforge::json::array(
                         {{{"type", "at"}, {"target", {{"qq", "42"}}}}, {{"type", "text"}, {"text", "在吗"}}})}}});
        const auto mention = drogon::sync_wait(insoulforge::MessageRouter::route(100, mentionSnapshot));
        check(mention.shouldReply, "bot mention replies without LLM", kTestName);
        check(mention.isPriority, "bot mention is priority", kTestName);

        const insoulforge::json shortSnapshot = insoulforge::json::array(
          {{{"sender", {{"qq", "11"}}}, {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "嗯"}}})}}});
        const auto shortMessage = drogon::sync_wait(insoulforge::MessageRouter::route(100, shortSnapshot));
        check(!shortMessage.shouldReply, "short group message skips without LLM", kTestName);

        config.selfQQNumber = originalSelfId;
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
        const int prepareResult =
          sqlite3_prepare_v2(db, "SELECT sampled_frame_count FROM image_description_cache LIMIT 1", -1, &stmt, nullptr);
        check(prepareResult == SQLITE_OK, "v6 migration creates sampled frame count column", kTestName);
        sqlite3_finalize(stmt);
        const int jobPrepareResult = sqlite3_prepare_v2(
          db, "SELECT messages, attempt_count FROM memory_maintenance_jobs LIMIT 1", -1, &stmt, nullptr);
        check(jobPrepareResult == SQLITE_OK, "migration creates durable memory maintenance jobs", kTestName);
        sqlite3_finalize(stmt);
        const int affinityJobPrepareResult = sqlite3_prepare_v2(
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
        const int64_t firstJob =
          insoulforge::MemoryMaintenanceStore::enqueue(100, messages, insoulforge::json::array(), 2);
        const auto pending = insoulforge::MemoryMaintenanceStore::pendingSessionIds();
        check(pending == std::vector<uint64_t>{100}, "pending session survives storage boundary", kTestName);
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

        const int64_t secondJob =
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
        insoulforge::Config::instance().loadFromDatabase();
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
        const int originalContextLimit = config.contextWindowLimit;
        const int originalTriggerCount = config.memorySummaryTriggerCount;
        const int originalBatchSize = config.memorySummaryBatchSize;
        const int originalSummaryContextCount = config.memorySummaryContextCount;
        config.contextWindowLimit = 2;
        config.memorySummaryTriggerCount = 3;
        config.memorySummaryBatchSize = 2;
        config.memorySummaryContextCount = 1;

        const auto makeMessage = [](const int id) {
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

        check(first.messageSnapshot.size() == 1, "first snapshot stays immutable after later appends", kTestName);
        check(third.summaryBatch.has_value(), "reaches trigger by reserving a summary batch", kTestName);
        check(third.summaryBatch->messages.size() == 2, "summary batch has configured size", kTestName);
        check(
          third.summaryBatch->messages[0]["message_id"] == "1", "summary batch keeps chronological order", kTestName);
        check(third.summaryBatch->contextMessages.size() == 1, "summary batch includes configured context", kTestName);
        check(!fourth.summaryBatch, "does not create another summary while one is pending", kTestName);
        check(fourth.messageSnapshot.size() == 2, "snapshot retains configured recent window", kTestName);
        check(fourth.messageSnapshot[0]["message_id"] == "3", "snapshot starts at configured context limit", kTestName);
        check(fourth.messageSnapshot[1]["assets"]["images"][0]["source"]["url"] == "https://example.com/image.jpg",
          "complete image source remains available to tools", kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(100).empty(), "runtime append does not write database",
          kTestName);

        insoulforge::MemoryMaintenanceStore::enqueue(
          100, third.summaryBatch->messages, third.summaryBatch->contextMessages, third.summaryBatch->messages.size());
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
          restored.snapshot() == fourth.messageSnapshot, "startup restoration rebuilds retained snapshot", kTestName);

        config.contextWindowLimit = originalContextLimit;
        config.memorySummaryTriggerCount = originalTriggerCount;
        config.memorySummaryBatchSize = originalBatchSize;
        config.memorySummaryContextCount = originalSummaryContextCount;
        database.close();
    }

} // namespace

int main() {
    testNewWorkflowNormalizesOneBotEvent();
    testNewWorkflowNormalizesOneBotNotices();
    testNewWorkflowDetectsCommands();
    testMessageRecordSemanticQueries();
    testNewWorkflowRouterHardRules();
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
