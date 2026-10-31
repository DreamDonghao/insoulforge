/// @file MessageContractTests.cpp
/// @brief 消息链路的契约测试

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <drogon/utils/coroutine.h>
#include <openssl/evp.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/logging/LogBuffer.hpp>

#include <admin/access/AdminAccessToken.hpp>
#include <workflow/access/AdminStore.hpp>
#include <admin/http/AdminController.hpp>
#include <workflow/agent/ability/AsyncTaskManager.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <workflow/agent/memory/maintenance/PersonMemoryConsolidator.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/custom/LuaToolExecutor.hpp>
#include <workflow/agent/tools/custom/ToolStore.hpp>
#include <workflow/agent/tools/plugins/ActionToolsPlugin.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/history/ChatRecordManager.hpp>
#include <workflow/history/SharedConversationContext.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/QQNameDirectory.hpp>
#include <workflow/session/SessionStore.hpp>
#include <workflow/command/CommandProcessor.hpp>
#include <workflow/MessageList.hpp>
#include <workflow/MessageRouter.hpp>
#include <onebot/incoming/OneBotEventNormalizer.hpp>
#include <onebot/messaging/MessageFormatter.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/SessionWorkflowState.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/agent/llm/JevClient.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>
#include <workflow/agent/llm/usage/UsageStore.hpp>
#include <workflow/agent/tools/image/CharacterImageStore.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>
#include <workflow/message/image/ImageDescriptionStore.hpp>
#include <workflow/agent/tools/image/ImageGenerationService.hpp>

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

    void testLogCredentialRedaction() {
        constexpr std::string_view kTestName = "log credential redaction before sinks and publication";
        std::ostringstream output;
        const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
        const auto previous = spdlog::default_logger();
        struct RestoreLogger {
            std::shared_ptr<spdlog::logger> previous;
            ~RestoreLogger() { spdlog::set_default_logger(previous); }
        } restore{previous};
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("privacy-test", sink));
        const std::string secret = "fixture-logger-admin-secret";
        insoulforge::Logger::info(0, "Admin", "管理后台访问令牌（重启后失效）: " + secret);
        insoulforge::Logger::info(0, "Admin", "http://127.0.0.1:7778/index.html#token=" + secret);
        insoulforge::Logger::info(0, "PrivacyFixture", "ordinary log remains visible");
        check(output.str().find(secret) == std::string::npos, "log sinks never receive raw credentials", kTestName);
        check(output.str().find("ordinary log remains visible") != std::string::npos,
          "ordinary output is preserved", kTestName);
        check(insoulforge::LogBuffer::instance().query({.keyword = secret}).entries.empty(),
          "published logs cannot contain or match the credential", kTestName);
    }

    void testOneBotMessageFormatter() {
        constexpr std::string_view kTestName = "OneBot message formatter";
        using insoulforge::MessageFormatter::convertAtToCQCode;
        insoulforge::QQNameDirectory::setCustomName(81001, "FormatUser");
        insoulforge::QQNameDirectory::setCustomName(81002, "FormatUserLong");
        check(convertAtToCQCode("hello") == "hello", "plain text is unchanged", kTestName);
        check(convertAtToCQCode("@[QQ:81003]") == "[CQ:at,qq=81003]", "numeric mention is converted", kTestName);
        check(convertAtToCQCode("@FormatUserLong @FormatUser") == "[CQ:at,qq=81002] [CQ:at,qq=81001]",
          "longer names are matched first", kTestName);
        check(convertAtToCQCode("@FormatUserSuffix") == "@FormatUserSuffix",
          "partial name is not converted", kTestName);
        check(convertAtToCQCode("[CQ:at,qq=81001]") == "[CQ:at,qq=81001]",
          "existing CQ mention is unchanged", kTestName);
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
        auto cardEvent = body;
        for (const auto role: {"owner", "admin", "member"}) {
            auto roleEvent = body;
            roleEvent["sender"]["role"] = role;
            const auto roleRecord = insoulforge::OneBotEventNormalizer::normalize(roleEvent);
            check(roleRecord && (*roleRecord)["sender"]["role"] == role, "group role is preserved", kTestName);
            if (roleRecord) {
                check(insoulforge::MessageRecord::projectForAgent(*roleRecord)["sender"]["role"] == role,
                  "agent context includes group role", kTestName);
            }
            roleEvent.erase("group_id");
            roleEvent["message_type"] = "private";
            const auto privateRoleRecord = insoulforge::OneBotEventNormalizer::normalize(roleEvent);
            check(privateRoleRecord && !(*privateRoleRecord)["sender"].contains("role"),
              "private messages omit group role", kTestName);
        }
        for (const auto role: {"", "unknown"}) {
            auto roleEvent = body;
            roleEvent["sender"]["role"] = role;
            const auto roleRecord = insoulforge::OneBotEventNormalizer::normalize(roleEvent);
            check(roleRecord && !(*roleRecord)["sender"].contains("role"), "invalid group role is omitted", kTestName);
        }
        cardEvent["sender"]["card"] = "Group Alice";
        const auto cardRecord = insoulforge::OneBotEventNormalizer::normalize(cardEvent);
        check(
          cardRecord && (*cardRecord)["sender"]["card"] == "Group Alice" && (*cardRecord)["sender"]["name"] == "Alice",
          "group card is stored separately from nickname", kTestName);
        if (cardRecord) {
            check(!insoulforge::MessageRecord::projectForAgent(*cardRecord)["sender"].contains("card"),
              "disabled projection omits group card", kTestName);
            check(
              insoulforge::MessageRecord::projectForAgent(*cardRecord, false, true)["sender"]["card"] == "Group Alice",
              "enabled projection includes group card", kTestName);
            check((*cardRecord)["sender"]["card"] == "Group Alice", "projection preserves stored card", kTestName);
        }
        cardEvent.erase("group_id");
        cardEvent["message_type"] = "private";
        const auto privateRecord = insoulforge::OneBotEventNormalizer::normalize(cardEvent);
        check(
          privateRecord && !(*privateRecord)["sender"].contains("card"), "private messages omit group card", kTestName);
        cardEvent = body;
        cardEvent["sender"]["card"] = "";
        const auto emptyCardRecord = insoulforge::OneBotEventNormalizer::normalize(cardEvent);
        check(
          emptyCardRecord && !(*emptyCardRecord)["sender"].contains("card"), "empty group card is omitted", kTestName);
        selfEvent["message_id"] = 8;
        selfEvent["sender"] = {{"user_id", 42}, {"nickname", "Bot"}};
        const auto selfRecord = insoulforge::OneBotEventNormalizer::normalize(selfEvent);
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
        auto &db = insoulforge::Database::instance();
        check(db.initialize(":memory:").has_value(), "initializes database", kTestName);
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
        check(!insoulforge::CommandProcessor::isCommand(privateCommand), "private slash text is ordinary message",
          kTestName);
        auto privateMentionCommand = groupCommand;
        privateMentionCommand["session_id"] = insoulforge::SessionId::fromPrivateUser(11);
        check(!insoulforge::CommandProcessor::isCommand(privateMentionCommand),
          "private bot mention does not enable commands", kTestName);

        insoulforge::SessionStore::setCommandEnabled(100, false);
        check(!insoulforge::CommandProcessor::isCommand(groupCommand), "disabled commands use ordinary message path",
          kTestName);
        check(
          insoulforge::SessionStore::getSessionConfig(200).commandEnabled, "other groups remain enabled", kTestName);
        insoulforge::SessionStore::setCommandEnabled(insoulforge::SessionId::fromPrivateUser(11), false);
        check(
          !insoulforge::CommandProcessor::isCommand(privateCommand), "private commands remain unsupported", kTestName);
        insoulforge::SessionStore::setCommandEnabled(100, true);
        check(
          insoulforge::CommandProcessor::isCommand(groupCommand), "re-enabling restores command parsing", kTestName);

        config.selfQQNumber = originalSelfId;
        db.close();
    }

    void testDefaultPrivateSessionState() {
        constexpr std::string_view kTestName = "default private session state";
        auto &db = insoulforge::Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
          ("insoulforge-private-default-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        using namespace insoulforge;
        const auto disabled = SessionId::fromPrivateUser(11);
        const auto enabled = SessionId::fromPrivateUser(22);
        SessionStore::initializeSession(disabled, false);
        SessionStore::initializeSession(enabled, true);
        SessionStore::initializeSession(100, true);
        check(SessionStore::hasSessionConfig(disabled) && !SessionStore::isSessionEnabled(disabled) &&
                SessionStore::isSessionEnabled(enabled) && !SessionStore::isSessionEnabled(100),
          "default only enables new private sessions", kTestName);
        check(SessionStore::getAllSessionsWithStatus().size() == 2 &&
                !SessionStore::getSessionConfig(enabled).crossSessionSharing,
          "disabled private sessions are visible without enabling sharing", kTestName);
        SessionStore::initializeSession(disabled, true);
        SessionStore::initializeSession(enabled, false);
        check(!SessionStore::isSessionEnabled(disabled) && SessionStore::isSessionEnabled(enabled),
          "changing default preserves existing sessions", kTestName);
        SessionStore::disableSession(enabled);
        SessionStore::initializeSession(enabled, true);
        check(!SessionStore::isSessionEnabled(enabled), "manual disabling is not reverted", kTestName);
        const auto manual = SessionId::fromPrivateUser(33);
        SessionStore::enableSession(manual);
        SessionStore::updateSessionName(manual, "kept name");
        SessionStore::initializeSession(manual, false);
        check(SessionStore::isSessionEnabled(manual) && SessionStore::getSessionName(manual) == "kept name",
          "preconfigured status and name survive first message", kTestName);
        const auto legacy = SessionId::fromPrivateUser(44);
        SessionStore::saveSessionConfig(legacy, {.allMesCount = 42});
        SessionStore::initializeSession(legacy, true);
        check(!SessionStore::isSessionEnabled(legacy) && SessionStore::getSessionConfig(legacy).allMesCount == 42,
          "legacy disabled session is not auto-enabled", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value(), "reopens database", kTestName);
        SessionStore::initializeSession(disabled, true);
        check(!SessionStore::isSessionEnabled(disabled) && SessionStore::isSessionEnabled(manual),
          "restart preserves per-session choices", kTestName);
        db.close();
        std::filesystem::remove(path);
    }

    void testSessionReplyCoalescing() {
        constexpr std::string_view kTestName = "session reply coalescing";
        auto &db = insoulforge::Database::instance();
        check(db.initialize(":memory:").has_value(), "initializes database", kTestName);
        const auto triggerId = [](std::optional<insoulforge::ReplyRequest> request) -> std::optional<std::string> {
            return request ? std::optional{std::move(request->messageId)} : std::nullopt;
        };
        {
            insoulforge::SessionWorkflowState privateState(insoulforge::SessionId::fromPrivateUser(11), db,
              insoulforge::Config::instance());
            check(triggerId(privateState.requestReplyProcessing("first", true)) == "first",
              "idle private session starts immediately", kTestName);
            check(!privateState.requestReplyProcessing("second", true) &&
                    !privateState.requestReplyProcessing("third", true),
              "new private messages do not start concurrent replies", kTestName);
            check(triggerId(privateState.completeReplyProcessing()) == "third",
              "private messages coalesce into latest trigger", kTestName);
            check(!privateState.requestReplyProcessing("fourth", true),
              "next reply retains exclusive processing", kTestName);
            check(triggerId(privateState.completeReplyProcessing()) == "fourth" && !privateState.completeReplyProcessing(),
              "later messages are processed and pending trigger is consumed once", kTestName);
            check(triggerId(privateState.requestReplyProcessing("fifth", true)) == "fifth" &&
                    !privateState.completeReplyProcessing(),
              "private session becomes idle after draining", kTestName);

            insoulforge::SessionWorkflowState groupState(100, db, insoulforge::Config::instance());
            check(triggerId(groupState.requestReplyProcessing("first", false)) == "first" &&
                    !groupState.requestReplyProcessing("ordinary", false) && !groupState.completeReplyProcessing(),
              "ordinary group messages do not schedule another reply", kTestName);
            check(triggerId(groupState.requestReplyProcessing("next", false)) == "next" &&
                    !groupState.requestReplyProcessing("mention", true) &&
                    !groupState.requestReplyProcessing("ordinary", false) &&
                    triggerId(groupState.completeReplyProcessing()) == "mention" && !groupState.completeReplyProcessing(),
              "eligible group trigger is preserved across ordinary messages", kTestName);

            check(!groupState.isReplyProcessing(), "drained session is idle", kTestName);
            const auto first = groupState.requestReplyProcessing("active", false);
            check(first && !first->decision && groupState.isReplyProcessing(), "starts without cached route", kTestName);
            insoulforge::RouterDecision decision;
            decision.reason = "new topic";
            decision.maxLength = 50;
            check(!groupState.requestReplyProcessing("routed", true, decision),
              "positive route waits for current reply", kTestName);
            check(!groupState.requestReplyProcessing("skipped", false),
              "skipped message preserves positive pending route", kTestName);
            const auto routed = groupState.completeReplyProcessing();
            check(routed && routed->messageId == "routed" && routed->decision &&
                    routed->decision->reason == "new topic" && routed->decision->maxLength == 50 &&
                    groupState.isReplyProcessing(), "next reply reuses decision with exclusive ownership", kTestName);
            check(!groupState.completeReplyProcessing(), "cached decision consumed once", kTestName);
            const auto late = groupState.requestReplyProcessing("late-route", true, decision);
            check(late && late->decision && late->messageId == "late-route",
              "route finishing after current reply starts immediately without losing decision", kTestName);
            check(!groupState.requestReplyProcessing("mention", true) &&
                    triggerId(groupState.completeReplyProcessing()) == "mention" && !groupState.completeReplyProcessing(),
              "later mandatory trigger replaces cached route and does not inherit its strategy", kTestName);
        }
        db.close();
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
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes session settings", kTestName);
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
        check(!mention.maxLength, "bot mention does not suggest a reply length", kTestName);

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
            check(systemDecision.maxLength == 100, "system message keeps its reply length", kTestName);
        }
        const auto privateSystemEvent = insoulforge::OneBotEventNormalizer::normalize({{"post_type", "message"},
          {"message_type", "private"}, {"user_id", 11}, {"self_id", 42}, {"message_id", 1'790'000'000'000'002LL},
          {"sender", {{"user_id", insoulforge::SessionId::kSystemAccountId}, {"nickname", "系统异步任务"}}},
          {"message", insoulforge::json::array({{{"type", "text"}, {"data", {{"text", "任务失败"}}}}})}});
        check(privateSystemEvent && (*privateSystemEvent)["session_id"] == insoulforge::SessionId::fromPrivateUser(11),
          "async task result reaches the original private session", kTestName);

        config.selfQQNumber = originalSelfId;
        database.close();
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

        const insoulforge::json shortSnapshot = insoulforge::json::array({{{"message_id", "short"},
          {"sender", {{"qq", "11"}}}, {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "嗯"}}})}}});
        const auto shortMessage = drogon::sync_wait(insoulforge::MessageRouter::route(100, "short", shortSnapshot));
        check(shortMessage.shouldReply && shortMessage.reason == "Router 请求失败",
          "single-character message reaches the same routing fallback as ordinary text", kTestName);

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
          "CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT); "
          "INSERT INTO settings VALUES('custom_tool_python','python3'),('other','preserved')",
          nullptr, nullptr, nullptr);
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
        check(jobPrepareResult != SQLITE_OK, "migration removes memory maintenance jobs", kTestName);
        sqlite3_finalize(stmt);
        const i32 affinityJobPrepareResult = sqlite3_prepare_v2(
          db, "SELECT messages, attempt_count FROM affinity_maintenance_jobs LIMIT 1", -1, &stmt, nullptr);
        check(affinityJobPrepareResult != SQLITE_OK, "migration removes affinity maintenance jobs", kTestName);
        sqlite3_finalize(stmt);
        check(sqlite3_prepare_v2(db, "SELECT * FROM custom_tools", -1, &stmt, nullptr) != SQLITE_OK,
          "migration removes legacy tool storage", kTestName);
        sqlite3_finalize(stmt);
        insoulforge::SchemaMigrator::migrate(db);
        check(sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key='custom_tool_python'", -1, &stmt, nullptr) ==
                  SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_DONE,
          "upgrade removes obsolete Python setting", kTestName);
        sqlite3_finalize(stmt);
        check(sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key='other'", -1, &stmt, nullptr) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW,
          "upgrade preserves unrelated settings", kTestName);
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "CREATE TABLE custom_tools(id INTEGER); PRAGMA user_version=12", nullptr, nullptr, nullptr);
        insoulforge::SchemaMigrator::migrate(db);
        check(sqlite3_prepare_v2(db, "SELECT * FROM custom_tools", -1, &stmt, nullptr) != SQLITE_OK,
          "v12 upgrade removes legacy table", kTestName);
        sqlite3_finalize(stmt);
        check(sqlite3_exec(db,
                "ALTER TABLE person_memories RENAME COLUMN profile TO stable_memory;"
                "ALTER TABLE person_memories DROP COLUMN relationship;"
                "INSERT INTO person_memories VALUES(42, '[\"旧偏好\"]', '[\"旧互动\"]');"
                "INSERT INTO person_memory_entries(qq_number,source_session,content) VALUES(42,100,'新材料');"
                "INSERT INTO person_affinity VALUES(42,7);"
                "INSERT INTO person_memory_daily_state VALUES(1,'2026-10-23','');"
                "PRAGMA user_version=22", nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates populated v22 person fixture", kTestName);
        insoulforge::SchemaMigrator::migrate(db);
        insoulforge::SchemaMigrator::migrate(db);
        {
            const insoulforge::Statement person(db,
              "SELECT p.profile,p.relationship,p.recent_interactions,e.content,a.affinity,s.completed_day "
              "FROM person_memories p JOIN person_memory_entries e USING(qq_number) "
              "JOIN person_affinity a USING(qq_number) JOIN person_memory_daily_state s ON s.id=1 WHERE p.qq_number=42");
            check(person.step() && person.getText(0) == "[]" && person.getText(1) == "[]" &&
                    person.getText(2) == "[]" && person.getText(3) == "新材料" &&
                    person.getInt(4) == 7 && person.getText(5) == "2026-10-23",
              "v22 upgrade clears old categories and preserves existing entries, affinity and checkpoint", kTestName);
            const insoulforge::Statement migrated(db,
              "SELECT COUNT(*), SUM(source_session=0 AND content IN ('旧偏好','旧互动')) "
              "FROM person_memory_entries WHERE qq_number=42");
            check(migrated.step() && migrated.getInt(0) == 3 && migrated.getInt(1) == 2,
              "old memory text moves to pending once without duplication", kTestName);
        }
        check(sqlite3_exec(db,
                "INSERT INTO person_memories VALUES(43,'[\"档案原文\"]','[\"互动原文\"]','[\"关系原文\"]');"
                "INSERT INTO person_memories(qq_number) VALUES(44);"
                "INSERT INTO person_memory_entries(qq_number,source_session,content,created_at) "
                "VALUES(43,200,'已有条目','2026-10-20 01:02:03');"
                "CREATE TRIGGER reject_history_migration BEFORE INSERT ON person_memory_entries "
                "WHEN NEW.content='关系原文' BEGIN SELECT RAISE(ABORT,'test'); END;"
                "PRAGMA user_version=23", nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates populated v23 migration failure fixture", kTestName);
        bool migrationFailed = false;
        try { insoulforge::SchemaMigrator::migrate(db); }
        catch (const insoulforge::DbError &) { migrationFailed = true; }
        {
            const insoulforge::Statement original(db,
              "SELECT profile,relationship,recent_interactions,"
              "(SELECT COUNT(*) FROM person_memory_entries WHERE qq_number=43) "
              "FROM person_memories WHERE qq_number=43");
            check(migrationFailed && original.step() && original.getText(0) == "[\"档案原文\"]" &&
                    original.getText(1) == "[\"关系原文\"]" && original.getText(2) == "[\"互动原文\"]" &&
                    original.getInt(3) == 1,
              "migration insert failure rolls back all enqueues and preserves old categories", kTestName);
        }
        sqlite3_exec(db, "DROP TRIGGER reject_history_migration", nullptr, nullptr, nullptr);
        insoulforge::SchemaMigrator::migrate(db);
        insoulforge::SchemaMigrator::migrate(db);
        {
            const insoulforge::Statement migrated(db,
              "SELECT COUNT(*), SUM(source_session=0 AND content IN ('档案原文','关系原文','互动原文')),"
              "SUM(source_session=200 AND content='已有条目' AND created_at='2026-10-20 01:02:03') "
              "FROM person_memory_entries WHERE qq_number=43");
            check(migrated.step() && migrated.getInt(0) == 4 && migrated.getInt(1) == 3 && migrated.getInt(2) == 1,
              "v23 upgrade moves all categories once and preserves existing pending metadata", kTestName);
            const insoulforge::Statement empty(db, "SELECT COUNT(*) FROM person_memory_entries WHERE qq_number=44");
            check(empty.step() && empty.getInt(0) == 0, "empty old categories create no pending entries", kTestName);
        }
        sqlite3_close(db);
    }

    void testFileToolStore() {
        constexpr std::string_view kTestName = "file custom tool storage";
        namespace store = insoulforge::ToolStore;
        const auto directory =
          std::filesystem::temp_directory_path() /
          ("insoulforge-tools-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto &database = insoulforge::Database::instance();
        check(database.initialize((directory / "bot.db").string()).has_value(), "opens file database", kTestName);
        auto tool = store::CustomTool{.name = "test_lua",
          .description = "test",
          .parameters = R"({"type":"object"})",
          .executorType = "lua",
          .scriptContent = "function run() return 'hello' end"};
        tool.id = store::addCustomTool(tool);
        check(std::filesystem::exists(directory / "tools/test_lua/main.lua"), "writes separate script", kTestName);
        check(store::getCustomTools().size() == 1 && store::getCustomTools()[0].scriptContent == tool.scriptContent,
          "reads tool metadata and script", kTestName);
        store::toggleCustomTool(tool.id);
        check(store::getEnabledCustomTools(database).empty(), "disabled tool is not registered", kTestName);
        tool.name = "renamed_tool";
        store::updateCustomTool(tool);
        check(!std::filesystem::exists(directory / "tools/test_lua") && store::hasCustomTool("renamed_tool"),
          "rename preserves ID and moves directory", kTestName);
        std::filesystem::rename(
          directory / "tools/renamed_tool", directory / ("tools/.backup-" + std::to_string(tool.id)));
        check(store::getCustomTools().size() == 1 && std::filesystem::exists(directory / "tools/renamed_tool"),
          "recovers interrupted directory replacement", kTestName);
        tool.parameters = "invalid JSON";
        try {
            store::updateCustomTool(tool);
            check(false, "rejects invalid metadata", kTestName);
        } catch (const std::exception &) {
            check(store::getCustomTools()[0].parameters == R"({"type":"object"})", "failed save preserves old tool",
              kTestName);
        }
        try {
            auto invalid = tool;
            invalid.name = "../escape";
            std::ignore = store::addCustomTool(invalid);
            check(false, "rejects path traversal", kTestName);
        } catch (const std::exception &) {
            check(!std::filesystem::exists(directory / "escape"), "no files outside tool directory", kTestName);
        }
        std::filesystem::create_directory_symlink(directory / "tools/renamed_tool", directory / "tools/symlink_tool");
        check(store::getCustomTools().size() == 1, "skips symlink tool directory", kTestName);
        database.close();
        check(database.initialize((directory / "bot.db").string()).has_value() && store::getCustomTools().size() == 1,
          "restart loads tools without database tool table", kTestName);
        store::deleteCustomTool(tool.id);
        tool.name = "new_tool";
        tool.parameters = "{}";
        const auto newId = store::addCustomTool(tool);
        check(newId > tool.id, "deleted IDs are not reused", kTestName);
        auto httpTool = tool;
        httpTool.name = "legacy_http";
        httpTool.executorType = "http";
        try {
            std::ignore = store::addCustomTool(httpTool);
            check(false, "rejects HTTP executor", kTestName);
        } catch (const std::exception &) {
            check(!std::filesystem::exists(directory / "tools/legacy_http"), "HTTP tool is not saved", kTestName);
        }
        std::filesystem::create_directory(directory / "tools/legacy_http");
        {
            std::ofstream metadata(directory / "tools/legacy_http/tool.json");
            metadata << R"({"id":999,"name":"legacy_http","description":"old","executorType":"http"})";
        }
        check(store::getCustomTools().size() == 1 && std::filesystem::exists(directory / "tools/legacy_http/tool.json"),
          "legacy HTTP tool is skipped without deleting user files", kTestName);
        auto pythonTool = tool;
        pythonTool.name = "legacy_python";
        pythonTool.executorType = "python";
        try {
            std::ignore = store::addCustomTool(pythonTool);
            check(false, "rejects Python executor", kTestName);
        } catch (const std::exception &) {
            check(!std::filesystem::exists(directory / "tools/legacy_python"), "Python tool is not saved", kTestName);
        }
        std::filesystem::create_directory(directory / "tools/legacy_python");
        {
            std::ofstream metadata(directory / "tools/legacy_python/tool.json");
            metadata << R"({"id":998,"name":"legacy_python","description":"old","executorType":"python"})";
        }
        check(
          store::getCustomTools().size() == 1 && std::filesystem::exists(directory / "tools/legacy_python/tool.json"),
          "legacy Python tool is skipped without deleting user files", kTestName);
        database.close();
        std::filesystem::remove_all(directory);
    }

    void testLuaToolExamples() {
        constexpr std::string_view kTestName = "Lua tool examples";
        const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
        const auto load = [&root](const std::string &name) -> std::string {
            std::ifstream stream(root / "agentTools" / (name + ".json"));
            insoulforge::json definition;
            stream >> definition;
            return definition.at("scriptContent").get<std::string>();
        };
        for (const auto &name: {"fetch_webpage", "search_web", "random", "get_weather", "deepseek_search", "deepseek_fetch"}) {
            check(!insoulforge::LuaToolExecutor::validate(load(name)), "valid Lua example", kTestName);
        }
        const auto execute = [&load](const std::string &name, insoulforge::json args) -> std::string {
            return drogon::sync_wait(
              insoulforge::LuaToolExecutor::execute(load(name), std::move(args), 123, false, true));
        };
        check(execute("random", {{"min", 4}, {"max", 4}}).find('4') != std::string::npos,
          "random example runs without Python", kTestName);
        check(execute("fetch_webpage", {{"url", "ftp://example.com"}}).starts_with("错误"),
          "webpage example rejects unsupported schemes", kTestName);
        check(execute("fetch_webpage", {{"url", "example.com"}}).find("https://example.com") != std::string::npos,
          "webpage example supplies HTTPS in mocked request body", kTestName);
        check(execute("search_web", {{"query", "a &b"}}).find("a+%26b") != std::string::npos,
          "search example encodes query", kTestName);
        check(execute("get_weather", {{"city", "New York"}}) == "天气接口响应格式无效",
          "weather example validates mocked response", kTestName);
        check(execute("deepseek_search", {{"query", "   "}}).starts_with("错误"),
          "DeepSeek search rejects blank queries", kTestName);
        check(execute("deepseek_search", {{"query", "test"}, {"count", 51}}).starts_with("错误"),
          "DeepSeek search rejects invalid result counts", kTestName);
        check(execute("deepseek_search", {{"query", "test"}}).find("API Key") != std::string::npos,
          "DeepSeek search requires a configured key", kTestName);
        check(execute("deepseek_fetch", {{"url", "ftp://example.com"}}).starts_with("错误"),
          "DeepSeek fetch rejects unsupported schemes", kTestName);
        check(execute("deepseek_fetch", {{"url", "https://user:pass@example.com"}}).starts_with("错误"),
          "DeepSeek fetch rejects embedded credentials", kTestName);
        check(execute("deepseek_fetch", {{"url", "example.com"}}).find("API Key") != std::string::npos,
          "DeepSeek fetch accepts addresses without a scheme", kTestName);
        for (const auto &name: {"deepseek_search", "deepseek_fetch"}) {
            const auto script = load(name) + R"lua(
                function run(args, ctx)
                    local text = string.rep("中", args.count)
                    local result = limit_output(text)
                    return json.encode({
                        length = utf8.len(result),
                        unchanged = result == text,
                        truncated = result:find("内容已截断", 1, true) ~= nil
                    })
                end
            )lua";
            for (const auto count: {19999, 20000, 20001, 50000}) {
                const auto output = drogon::sync_wait(
                  insoulforge::LuaToolExecutor::execute(script, {{"count", count}}, 123, false, true));
                const auto result = insoulforge::json::parse(output);
                check(result["length"] == std::min(count, 20000), "DeepSeek output respects character cap", kTestName);
                check(result["unchanged"] == (count <= 20000), "DeepSeek preserves output within cap", kTestName);
                check(result["truncated"] == (count > 20000), "DeepSeek marks truncated output", kTestName);
            }
        }
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
        const auto utilities = drogon::sync_wait(insoulforge::LuaToolExecutor::execute(R"lua(
            function run()
                local value = json.decode('{"items":[],"missing":null,"text":"你好"}')
                assert(value.missing == json.null)
                local body = json.encode(value)
                assert(body:find('"items":[]', 1, true))
                assert(body:find('"missing":null', 1, true))
                local response = http.request({url="https://example.com?query=test#fragment",
                    method="POST", headers={['Content-Type']='application/json'}, body=body})
                assert(response.ok and response.status == 200 and response.body == body)
                assert(not http.request({url="file:///tmp/test"}).ok)
                assert(not http.request({url="https://example.com", timeout_ms=0}).ok)
                assert(not http.request({url="https://user:pass@example.com"}).ok)
                assert(not pcall(json.decode, '{bad'))
                return url.encode("a b&c")
            end
        )lua",
          insoulforge::json::object(), 123, false, true));
        check(utilities == "a+b%26c", "HTTP validation and JSON/URL helpers", kTestName);
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
        const auto registerVersion = [&registry](std::string pluginId, std::string result) -> bool {
            return registry.registerPlugin(std::move(pluginId), [&result](insoulforge::ToolRegistry &staged) -> void {
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
                [&](insoulforge::ToolRegistry &staged) -> void {
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

        const auto names = [](const insoulforge::json &tools) -> std::vector<std::string> {
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
          {{{"role", "system"}, {"content", "original prompt"}},
            {{"role", "user"}, {"content", R"({"response_requirements":{"max_length":100},"recent_messages":[]})"}},
            {{"role", "assistant"}, {"tool_calls", {{{"id", "call-1"}, {"type", "function"}}}}},
            {{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "source data"}}});

        const auto first = insoulforge::ExecutorAgent::buildIterationMessages(history, 1, 8);
        const auto middle = insoulforge::ExecutorAgent::buildIterationMessages(history, 3, 8);
        const auto last = insoulforge::ExecutorAgent::buildIterationMessages(history, 8, 8);
        check(first.size() == history.size() + 1 && middle.size() == first.size() && last.size() == first.size(),
          "each request adds one transient execution-state message", kTestName);
        for (size_t index = 0; index < history.size(); ++index) {
            check(first[index] == history[index] && middle[index] == history[index] && last[index] == history[index],
              "request status preserves the full history and tool results", kTestName);
        }
        const auto context = [](const insoulforge::json &messages) {
            return insoulforge::json::parse(messages.back()["content"].get<std::string>());
        };
        const auto firstContext = context(first);
        check(firstContext["execution_state"]["remaining_rounds"] == 7 &&
                firstContext["execution_state"]["reply_only"] == false,
          "first round counts future requests excluding the current one", kTestName);
        check(context(middle)["execution_state"]["current_round"] == 3 &&
                context(middle)["execution_state"]["remaining_rounds"] == 5,
          "intermediate status reflects the current round", kTestName);
        check(context(last)["execution_state"]["remaining_rounds"] == 0 &&
                context(last)["execution_state"]["reply_only"] == true,
          "last round explicitly requires a reply decision", kTestName);
        const auto single = insoulforge::ExecutorAgent::buildIterationMessages(history, 1, 1);
        check(context(single)["execution_state"]["reply_only"] == true,
          "a one-round limit is immediately reply-only", kTestName);
        check(first.back()["role"] == "system" && firstContext.size() == 1,
          "transient system message contains structured data only", kTestName);
        const insoulforge::json mcp = {{"page_size", 10}, {"max_per_enable", 4},
          {"max_per_reply", 12}, {"selected_count", 2}};
        const auto withMcp = insoulforge::ExecutorAgent::buildIterationMessages(history, 2, 8, mcp);
        check(context(withMcp)["execution_state"]["mcp"] == mcp &&
                context(withMcp)["execution_state"]["current_round"] == 2,
          "each request replaces stale execution state and includes current MCP limits", kTestName);
        const auto finalMcp = insoulforge::ExecutorAgent::buildIterationMessages(history, 8, 8, mcp);
        check(!context(finalMcp)["execution_state"].contains("mcp") &&
                !firstContext["execution_state"].contains("mcp"),
          "MCP status is absent when unavailable or reply-only", kTestName);
        check(history.size() == 4 && history.front()["content"] == "original prompt",
          "transient status never modifies the original conversation", kTestName);
    }

    void testToolRegistryReturnsHandlerErrors() {
        constexpr std::string_view kTestName = "tool handler error result";
        auto &registry = insoulforge::ToolRegistry::instance();
        check(registry.registerPlugin("contract.error",
                [](insoulforge::ToolRegistry &staged) -> void {
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
          reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size());
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
                [](insoulforge::ToolRegistry &staged) -> void {
                    insoulforge::ActionToolsPlugin{"test-bot"}.registerTools(staged);
                }),
          "action tools register", kTestName);
        const auto oldConfig = insoulforge::Config::instance().imageGeneration;
        auto &api = insoulforge::Config::instance().imageGeneration;
        api.apiKey = "test";
        api.baseUrl = "https://example.com";
        api.path = "/images";
        api.model = "test";
        bool foundGeneration = false;
        for (const auto &tool : registry.getTools({})) {
            const auto &definition = tool["function"];
            check(definition["name"] != "edit_image", "old edit tool is removed", kTestName);
            if (definition["name"] == "generate_image") {
                foundGeneration = true;
                check(definition["parameters"]["required"] == insoulforge::json::array({"prompt"}) &&
                    definition["parameters"]["properties"]["reference_images"]["type"] == "array",
                  "unified tool accepts optional reference array", kTestName);
            }
        }
        check(foundGeneration, "unified image tool exists", kTestName);
        for (const auto &images : {insoulforge::json(nullptr), insoulforge::json("url"),
                 insoulforge::json::array({42}),
                 insoulforge::json::array({{{"message_id", "1"}, {"image_index", -1}}}),
                 insoulforge::json::array({{{"message_id", "1"}, {"image_index", 0.5}}}),
                 insoulforge::json::array({{{"message_id", "1"}, {"image_index", "0"}}})}) {
            const auto error = drogon::sync_wait(registry.executeTool("generate_image",
              {{"prompt", "参考图片"}, {"reference_images", images}}, {.sessionId = 123}));
            check(error.find("数组") != std::string::npos || error.find("整数图片索引") != std::string::npos,
              "invalid references reject before task starts", kTestName);
        }
        const auto result = drogon::sync_wait(registry.executeTool(
          "generate_image", {{"prompt", "画出你自己"}, {"use_self_reference", true}}, {.sessionId = 123}));
        check(result.find("尚未在管理后台上传") != std::string::npos, "missing image rejects before task starts",
          kTestName);
        api = oldConfig;
        registry.unregisterPlugin("contract.character-image");
    }

    void testQzoneToolValidation() {
        constexpr std::string_view kTestName = "qzone tool validation";
        auto &registry = insoulforge::ToolRegistry::instance();
        check(registry.registerPlugin("contract.qzone", [](insoulforge::ToolRegistry &staged) -> void {
            insoulforge::ActionToolsPlugin{"test-bot"}.registerTools(staged);
        }), "action tools register", kTestName);
        for (const auto &tool : registry.getTools({})) {
            if (tool["function"]["name"] == "send_qzone_msg")
                check(tool["function"]["parameters"]["properties"]["ugc_right"]["default"] == 1,
                  "qzone default visibility is public", kTestName);
        }
        const auto reject = [&](const insoulforge::json &args, const std::string_view expected) -> void {
            const auto result = drogon::sync_wait(registry.executeTool("send_qzone_msg", args, {.sessionId = 123}));
            check(result.find(expected) != std::string::npos, "invalid arguments reject before publishing", kTestName);
        };
        reject({{"content", " \n "}}, "非空");
        reject({{"content", "test"}, {"ugc_right", 16}}, "ugc_right");
        reject({{"content", "test"}, {"ugc_right", "4"}}, "ugc_right");
        reject({{"content", "test"}, {"images", "https://example.com/a.png"}}, "数组");
        reject({{"content", "test"}, {"images", insoulforge::json::array({42})}}, "HTTP/HTTPS");
        reject({{"content", "test"}, {"images", {"file:///tmp/a.png"}}}, "HTTP/HTTPS");
        reject({{"content", "test"}, {"images", {"https:///a.png"}}}, "主机名");
        reject({{"content", "test"}, {"images", {"https://"}}}, "主机名");
        reject({{"content", "test"}, {"images", {"https://example.com/a b.png"}}}, "HTTP/HTTPS");
        insoulforge::json tooMany = insoulforge::json::array();
        for (int i = 0; i < 10; ++i) {
            tooMany.push_back("https://example.com/a.png");
        }
        reject({{"content", "test"}, {"images", tooMany}}, "9");
        registry.unregisterPlugin("contract.qzone");
    }

    void testBuiltinToolConfig() {
        constexpr std::string_view kTestName = "builtin tool config";
        auto &registry = insoulforge::ToolRegistry::instance();
        auto &config = insoulforge::Config::instance();
        const auto path = std::filesystem::temp_directory_path() /
          ("insoulforge-builtin-tools-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        check(config.initialize(path.string()).has_value(), "initializes config", kTestName);
        int executed = 0;
        const auto registrar = [&](insoulforge::ToolRegistry &staged) {
            staged.registerTool({.name = "contract_builtin", .description = "default description",
              .handler = [&](insoulforge::json, insoulforge::ToolCallContext) -> drogon::Task<std::string> {
                  ++executed;
                  co_return "executed";
              }}, insoulforge::ToolCategory::REPLY);
        };
        check(registry.registerPlugin("builtin.contract-config", registrar), "registers builtin", kTestName);
        const auto definition = [&]() -> insoulforge::json {
            for (const auto &tool : registry.getTools({})) {
                if (tool["function"]["name"] == "contract_builtin") return tool["function"];
            }
            return nullptr;
        };
        check(definition()["description"] == "default description", "default description preserved", kTestName);
        check(!registry.isToolDisabled("missing_builtin"), "unknown tool is not explicitly disabled", kTestName);
        check(registry.saveBuiltinToolConfig("contract_builtin", {{"enabled", false}, {"description", "custom description"}}).has_value(),
          "saves disabled tool", kTestName);
        check(definition().is_null() && registry.hasTool("contract_builtin") && registry.isToolDisabled("contract_builtin"),
          "disabled tool hidden but name remains reserved", kTestName);
        check(drogon::sync_wait(registry.executeTool("contract_builtin", {}, {})).find("已关闭") != std::string::npos && executed == 0,
          "disabled tool cannot execute", kTestName);
        check(config.initialize(path.string()).has_value(), "reloads file", kTestName);
        registry.loadBuiltinToolsConfig(config.getBuiltinToolsConfig());
        check(registry.registerPlugin("builtin.contract-config", registrar) && registry.isToolDisabled("contract_builtin"),
          "reload preserves disabled state", kTestName);
        check(registry.saveBuiltinToolConfig("contract_builtin", {{"enabled", true}}).has_value() &&
          definition()["description"] == "custom description", "custom description reaches model", kTestName);
        check(drogon::sync_wait(registry.executeTool("contract_builtin", {}, {})) == "executed" && executed == 1,
          "enabled tool executes", kTestName);
        check(!registry.saveBuiltinToolConfig("contract_builtin", {{"enabled", false}, {"description", 42}}) && !registry.isToolDisabled("contract_builtin"),
          "invalid config preserves runtime state", kTestName);
        check(!registry.saveBuiltinToolConfig("contract_builtin", {{"description", std::string(16001, 'a')}}) &&
          !registry.isToolDisabled("contract_builtin"), "oversized description preserves state", kTestName);
        check(!registry.saveBuiltinToolConfig("missing_builtin", {{"enabled", false}}), "unknown tool rejected", kTestName);
        check(registry.saveBuiltinToolConfig("contract_builtin", {{"description", nullptr}}).has_value() &&
          definition()["description"] == "default description", "null restores default description", kTestName);
        bool listed = false;
        for (const auto &tool : registry.getBuiltinTools()) {
            if (tool["name"] == "contract_builtin") {
                listed = tool["enabled"] == true && tool["customDescription"] == false && tool["category"] == "回复";
            }
        }
        check(listed, "management list includes default metadata", kTestName);
        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const insoulforge::json &body) {
            const auto request = drogon::HttpRequest::newHttpRequest();
            request->setBody(insoulforge::dumpJson(body));
            drogon::sync_wait(controller.saveBuiltinTool(request, [&](const auto &value) { response = value; }));
            return insoulforge::parseJson(std::string(response->body()));
        };
        check(save({{"name", "contract_builtin"}, {"enabled", false}, {"description", "edited via API"}})["success"] == true &&
          registry.isToolDisabled("contract_builtin"), "endpoint applies setting", kTestName);
        check(save({{"name", "contract_builtin"}, {"enabled", "true"}, {"description", nullptr}}).contains("error") &&
          registry.isToolDisabled("contract_builtin"), "endpoint rejects invalid enabled type", kTestName);
        check(save({{"name", "contract_builtin"}, {"description", "description-only edit"}})["success"] == true &&
          registry.isToolDisabled("contract_builtin"), "description edit preserves disabled state", kTestName);
        check(save({{"name", "contract_builtin"}, {"enabled", true}})["success"] == true &&
          definition()["description"] == "description-only edit", "toggle preserves edited description", kTestName);
        check(save({{"name", "contract_builtin"}, {"description", nullptr}})["success"] == true &&
          !registry.isToolDisabled("contract_builtin") && definition()["description"] == "default description",
          "restore preserves enabled state", kTestName);
        check(save({{"name", "contract_builtin"}}).contains("error"), "empty patch rejected", kTestName);
        check(save({{"name", "contract_builtin"}, {"parameters", nullptr}}).contains("error"), "unknown field rejected", kTestName);
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        check(!registry.saveBuiltinToolConfig("contract_builtin", {{"enabled", false}}) &&
          !registry.isToolDisabled("contract_builtin"), "write failure preserves runtime state", kTestName);
        registry.unregisterPlugin("builtin.contract-config");
        registry.loadBuiltinToolsConfig(insoulforge::json::object());
        std::filesystem::remove(path);
    }

    void testMemoryMaintenanceStore() {
        constexpr std::string_view kTestName = "memory maintenance results";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        const auto noTaskTables = [&]() -> bool {
            sqlite3_stmt *stmt = nullptr;
            const bool prepared = sqlite3_prepare_v2(database.handle(),
                                    "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                                    "('memory_maintenance_jobs','affinity_maintenance_jobs')",
                                    -1, &stmt, nullptr) == SQLITE_OK;
            const bool absent = prepared && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0;
            sqlite3_finalize(stmt);
            return absent;
        };
        check(noTaskTables(), "fresh schema has no maintenance queues", kTestName);
        const insoulforge::PreparedLongTermMemory longTerm{
          .content = "Alice 喜欢 C++", .embedding = {0.1F, 0.2F}};
        insoulforge::MemoryMaintenanceStore::apply(
          database, 100, {.shortTerm = "近期状态", .longTerm = {longTerm},
            .personEntries = {{11, "喜欢 C++"}}, .affinity = {{11, 3}}});
        const auto personSenders = insoulforge::json::array({{{"sender", {{"qq", "11"}}}}});
        const auto initialProfiles = insoulforge::PersonMemoryStore::getForSenders(personSenders);
        const auto resultsPreserved = [&]() -> bool {
            return insoulforge::MemoryStore::getShortTermMemory(100) == "近期状态" &&
                   insoulforge::LongTermMemoryStore::countMemories(100) == 1 &&
                   insoulforge::AffinityStore::getAffinityMap()[11] == 3 &&
                   insoulforge::PersonMemoryStore::getForSenders(personSenders) == initialProfiles;
        };
        check(resultsPreserved(), "commits memory and affinity together", kTestName);
        check(sqlite3_exec(database.handle(),
                "CREATE TRIGGER reject_affinity BEFORE INSERT ON person_affinity BEGIN SELECT RAISE(ABORT, 'test'); END",
                nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates failure fixture", kTestName);
        bool failed = false;
        try {
            insoulforge::MemoryMaintenanceStore::apply(
              database, 100, {.shortTerm = "不能保存", .longTerm = {longTerm},
                .personEntries = {{11, "不能保存"}}, .affinity = {{12, 5}}});
        } catch (const insoulforge::DbError &) {
            failed = true;
        }
        check(failed && resultsPreserved(), "affinity failure rolls back all memory writes", kTestName);
        // 删除旧记忆后发生的写入失败也必须恢复原 ID，不能只回滚新条目。
        const auto existingId = insoulforge::LongTermMemoryStore::listMemories(100, 1, 0).front().id;
        failed = false;
        try {
            insoulforge::MemoryMaintenanceStore::apply(database, 100,
              {.shortTerm = "不能保存", .longTerm = {longTerm}, .replacedLongTermIds = {existingId},
                .affinity = {{12, 5}}});
        } catch (const insoulforge::DbError &) {
            failed = true;
        }
        check(failed && resultsPreserved() &&
                insoulforge::LongTermMemoryStore::listMemories(100, 1, 0).front().id == existingId,
          "failed commit restores deleted old IDs and original session memory", kTestName);
        check(sqlite3_exec(database.handle(), "DROP TRIGGER reject_affinity", nullptr, nullptr, nullptr) == SQLITE_OK,
          "removes failure fixture", kTestName);
        insoulforge::MemoryMaintenanceStore::apply(database, 100, {});
        check(resultsPreserved(), "empty result preserves saved memory", kTestName);
        const auto memory = drogon::sync_wait(
          insoulforge::MemoryMaintenanceService::prepare(100, insoulforge::json::array(), insoulforge::json::array()));
        check(memory.has_value() && memory->affinity.empty(), "empty input is a successful empty calculation",
          kTestName);
        check(sqlite3_exec(database.handle(),
                "CREATE TABLE memory_maintenance_jobs(id INTEGER PRIMARY KEY);"
                "INSERT INTO memory_maintenance_jobs VALUES(1);"
                "CREATE TABLE affinity_maintenance_jobs(id INTEGER PRIMARY KEY);"
                "INSERT INTO affinity_maintenance_jobs VALUES(1);"
                "PRAGMA user_version = 17",
                nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates v17 queues", kTestName);
        insoulforge::SchemaMigrator::migrate(database.handle());
        check(noTaskTables() && resultsPreserved(), "upgrade removes old tasks but preserves results", kTestName);
        insoulforge::MemoryMaintenanceStore::apply(database, 200, {.longTerm = {longTerm}});
        const auto oldEntries = insoulforge::LongTermMemoryStore::listMemories(200, 1, 0);
        const i64 sourceId = oldEntries.front().id;
        const insoulforge::PreparedLongTermMemory replacementA{
          .content = "Alice 喜欢编程", .embedding = {0.1F, 0.2F}};
        const insoulforge::PreparedLongTermMemory replacementB{
          .content = "Alice 使用 C++", .embedding = {0.2F, 0.3F}};
        insoulforge::MemoryMaintenanceStore::apply(database, 200, {.longTerm = {replacementA, replacementB}, .replacedLongTermIds = {sourceId, sourceId}});
        const auto replacements = insoulforge::LongTermMemoryStore::listMemories(200, 10, 0);
        check(replacements.size() == 2 &&
                std::ranges::none_of(replacements, [sourceId](const auto &entry) { return entry.id == sourceId; }),
          "store deduplicates replacement IDs across all new memories", kTestName);
        // 外号吸收了全部信息时，允许空向量结果独立删除旧条目；删除必须限定当前会话。
        insoulforge::MemoryMaintenanceStore::apply(database, 200,
          {.replacedLongTermIds = {replacements[0].id, replacements[1].id, existingId}});
        check(insoulforge::LongTermMemoryStore::countMemories(200) == 0 && resultsPreserved(),
          "empty replacement deletes supplied old IDs without affecting another session", kTestName);
        database.close();
    }

    void testGlobalPersonMemory() {
        constexpr std::string_view kTestName = "global person memory";
        using namespace insoulforge;
        auto &db = Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-person-memory-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        check(sqlite3_exec(db.handle(),
                "DROP TABLE person_affinity;"
                "CREATE TABLE group_affinity(group_id INTEGER, qq_number INTEGER, affinity INTEGER);"
                "INSERT INTO group_affinity VALUES(100,11,10),(200,11,11),(100,22,-3),(200,22,-4),(100,33,120);"
                "PRAGMA user_version = 20", nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates old per-session scores", kTestName);
        SchemaMigrator::migrate(db.handle());
        const auto scores = AffinityStore::getAffinityMap();
        check(scores.at(11) == 11 && scores.at(22) == -4 && scores.at(33) == 100,
          "migration averages existing rows, rounds halves away from zero and clamps", kTestName);
        AffinityStore::adjustAffinity(11, 2);
        SchemaMigrator::migrate(db.handle());
        check(AffinityStore::getAffinityMap().at(11) == 13, "migration does not reapply old scores", kTestName);
        const auto privateId = SessionId::fromPrivateUser(11);
        MemoryMaintenanceStore::apply(db, 100, {.personEntries = {{11, "喜欢摄影"}, {22, "喜欢编程"}}});
        MemoryMaintenanceStore::apply(db, privateId, {.personEntries = {{11, "准备比赛"}}, .affinity = {{11, -1}}});
        const auto senders = json::array({{{"sender", {{"qq", "11"}}}}, {{"sender", {{"qq", "11"}}}},
          {{"sender", {{"qq", "self"}}}}, {{"sender", {{"qq", std::to_string(SessionId::kSystemAccountId)}}}},
          {{"sender", {{"qq", "33"}}}}, {{"segments", {{{"type", "at"}, {"target", {{"qq", "22"}}}}}}}});
        const auto profiles = PersonMemoryStore::getForSenders(senders);
        check(profiles.size() == 1 && profiles[0]["qq"] == "11" && profiles[0]["pending_entries"].size() == 2 &&
                profiles[0]["pending_entries"][1]["source_session"] == std::to_string(privateId) &&
                AffinityStore::getAffinityMap().at(11) == 12,
          "deduplicates recent senders, excludes mentions and missing profiles, shares private entries globally", kTestName);
        check(!SessionStore::getSessionConfig(100).crossSessionSharing &&
                !SessionStore::getSessionConfig(privateId).crossSessionSharing,
          "person memory is independent of raw-message sharing", kTestName);
        MemoryMaintenanceStore::apply(db, 100, {.personEntries = {{44, "仅有人物记忆"}}});
        const auto people = PersonMemoryStore::listPeople();
        check(people.size() == 4 && people[0]["pending_count"] == 2 && !people[0].contains("pending_entries"),
          "admin summaries include memory-only and affinity-only people without loading memory bodies", kTestName);
        const auto person = PersonMemoryStore::getPerson(11);
        check(person["affinity"] == 12 && person["pending_entries"].size() == 2 &&
                person["pending_entries"][0]["source_session"] == std::to_string(privateId),
          "person detail combines global score and newest-first entries with exact session IDs", kTestName);
        check(profiles[0]["pending_entries"][0].contains("created_at") &&
                !profiles[0]["pending_entries"][0].contains("time") &&
                person["pending_entries"][1].contains("time") &&
                !person["pending_entries"][1].contains("created_at") &&
                profiles[0]["pending_entries"][0]["created_at"] == person["pending_entries"][1]["time"],
          "model uses created_at while admin API retains original timestamp contract", kTestName);
        check(PersonMemoryStore::getPerson(33)["profile"].empty() &&
                PersonMemoryStore::getPerson(44)["affinity"] == 0 && PersonMemoryStore::getPerson(999).is_null(),
          "handles score-only, memory-only and unknown profiles", kTestName);
        const AdminController controller;
        auto response = drogon::HttpResponsePtr{};
        const auto callback = [&](const drogon::HttpResponsePtr &value) { response = value; };
        for (const u64 qq: {11, 22, 33, 44})
            QQNameDirectory::recordName(qq, "Known " + std::to_string(qq));
        drogon::sync_wait(controller.getPeople(drogon::HttpRequest::newHttpRequest(), callback));
        check(json::parse(response->body()).size() == 4, "people API returns all profiles", kTestName);
        drogon::sync_wait(controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "11"));
        check(json::parse(response->body())["qq"] == "11", "person API returns string identity", kTestName);
        drogon::sync_wait(controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "invalid"));
        check(json::parse(response->body()).contains("error"), "person API rejects invalid identities", kTestName);
        std::thread first([] { for (int index = 0; index < 50; ++index) AffinityStore::adjustAffinity(77, 1); });
        std::thread second([] { for (int index = 0; index < 50; ++index) AffinityStore::adjustAffinity(77, 1); });
        first.join();
        second.join();
        AffinityStore::adjustAffinity(77, 10);
        check(AffinityStore::getAffinityMap().at(77) == 100, "concurrent global increments do not overwrite or overflow", kTestName);
        {
            const Statement query(db.handle(), "SELECT COUNT(*) FROM sqlite_master WHERE name = 'group_affinity'");
            check(query.step() && query.getInt(0) == 0, "old per-session affinity table is removed", kTestName);
        }
        db.close();
        check(db.initialize(path.string()).has_value() && PersonMemoryStore::getForSenders(senders) == profiles,
          "person entries survive restart", kTestName);
        db.close();
        std::filesystem::remove(path);
    }

    void testPersonConsolidationStore() {
        constexpr std::string_view kTestName = "person consolidation store and calendar";
        using namespace insoulforge;
        const auto localTime = [](int year, int month, int day, int hour, int minute = 0, int second = 0) {
            std::tm value{};
            value.tm_year = year - 1900;
            value.tm_mon = month - 1;
            value.tm_mday = day;
            value.tm_hour = hour;
            value.tm_min = minute;
            value.tm_sec = second;
            value.tm_isdst = -1;
            return std::mktime(&value);
        };
        const auto due = PersonMemoryConsolidator::dueDay;
        check(!due(localTime(2026, 10, 20, 2, 59, 59), {}) &&
                due(localTime(2026, 10, 20, 3), {}) == "2026-10-20",
          "first run waits until local 03:00 exactly", kTestName);
        check(!due(localTime(2026, 10, 20, 23), {"2026-10-20", ""}) &&
                !due(localTime(2026, 10, 21, 2), {"2026-10-20", ""}),
          "completed day is not repeated across midnight", kTestName);
        check(due(localTime(2027, 1, 1, 2), {"2026-12-29", ""}) == "2026-12-31" &&
                due(localTime(2026, 10, 22, 8), {"2026-10-19", "2026-10-20"}) == "2026-10-22" &&
                due(localTime(2026, 10, 21, 2), {"", "2026-10-20"}) == "2026-10-20",
          "missed dates coalesce and unfinished run resumes even before 03:00", kTestName);
        auto &db = Database::instance();
        check(db.initialize(":memory:").has_value(), "initializes database", kTestName);
        check(sqlite3_exec(db.handle(), "DROP TABLE person_memory_daily_state; PRAGMA user_version = 21",
                nullptr, nullptr, nullptr) == SQLITE_OK, "creates v21 upgrade fixture", kTestName);
        SchemaMigrator::migrate(db.handle());
        PersonMemoryStore::saveDailyState({"2026-10-19", "2026-10-20"});
        SchemaMigrator::migrate(db.handle());
        const auto state = PersonMemoryStore::dailyState();
        check(state.completedDay == "2026-10-19" && state.runningDay == "2026-10-20",
          "v22 migration and repeated migration preserve daily checkpoint", kTestName);
        MemoryMaintenanceStore::apply(db, 100, {.personEntries = {{11, "旧条目"}}, .affinity = {{11, 3}}});
        const auto snapshot = PersonMemoryStore::pendingSnapshot(11);
        check(snapshot.has_value(), "reads pending snapshot", kTestName);
        if (snapshot) {
            MemoryMaintenanceStore::apply(db, SessionId::fromPrivateUser(11), {.personEntries = {{11, "并发新增"}}});
            const json profile = {{"profile", {"喜欢摄影"}}, {"relationship", {"经常交流摄影"}},
              {"recent_interactions", {"准备一起拍照"}}};
            check(sqlite3_exec(db.handle(),
                    "CREATE TRIGGER reject_person_delete BEFORE DELETE ON person_memory_entries "
                    "BEGIN SELECT RAISE(ABORT, 'test'); END", nullptr, nullptr, nullptr) == SQLITE_OK,
              "creates delete failure trigger", kTestName);
            bool failed = false;
            try { PersonMemoryStore::applyConsolidation(*snapshot, profile); }
            catch (const DbError &) { failed = true; }
            check(failed && PersonMemoryStore::getPerson(11)["profile"].empty() &&
                    PersonMemoryStore::getPerson(11)["pending_entries"].size() == 2,
              "delete failure rolls back profile update and preserves entries", kTestName);
            check(sqlite3_exec(db.handle(), "DROP TRIGGER reject_person_delete", nullptr, nullptr, nullptr) == SQLITE_OK,
              "removes failure trigger", kTestName);
            PersonMemoryStore::applyConsolidation(*snapshot, profile);
            const auto person = PersonMemoryStore::getPerson(11);
            check(person["profile"] == profile["profile"] && person["pending_entries"].size() == 1 &&
                    person["relationship"] == profile["relationship"] &&
                    person["pending_entries"][0]["content"] == "并发新增" && person["affinity"] == 3 &&
                    MemoryStore::getShortTermMemory(100).empty(),
              "snapshot commit keeps new entries and does not touch affinity or session memory", kTestName);
        }
        check(!PersonMemoryStore::pendingSnapshot(999), "absent person has no work", kTestName);
        auto original = PersonMemoryStore::getPerson(11);
        auto edited = original;
        edited["affinity"] = -100;
        edited["profile"] = json::array();
        edited["pending_entries"][0]["content"] = "人工修正";
        const auto stale = PersonMemoryStore::pendingSnapshot(11);
        check(PersonMemoryStore::updatePerson(11, original, edited), "updates person atomically", kTestName);
        check(PersonMemoryStore::getPerson(11)["affinity"] == -100 &&
                PersonMemoryStore::getPerson(11)["pending_entries"][0]["content"] == "人工修正",
          "edits affinity and pending content", kTestName);
        check(!PersonMemoryStore::updatePerson(11, original, edited) &&
                !PersonMemoryStore::deletePerson(11, original), "rejects stale admin snapshots", kTestName);
        bool conflict = false;
        try { PersonMemoryStore::applyConsolidation(*stale,
            {{"profile", {"过期结果"}}, {"recent_interactions", json::array()}}); }
        catch (const PersonMemoryStore::ConsolidationConflict &) { conflict = true; }
        check(conflict && PersonMemoryStore::getPerson(11)["profile"].empty(),
          "daily consolidation cannot overwrite manual edits", kTestName);
        original = PersonMemoryStore::getPerson(11);
        edited = original;
        edited["pending_entries"] = json::array();
        check(PersonMemoryStore::updatePerson(11, original, edited) &&
                PersonMemoryStore::getPerson(11)["pending_entries"].empty(),
          "editing can remove pending entries", kTestName);
        MemoryMaintenanceStore::apply(db, 100, {.personEntries = {{11, "删除前条目"}, {22, "保留其他人"}}});
        const auto beforeDelete = PersonMemoryStore::pendingSnapshot(11);
        check(PersonMemoryStore::deletePerson(11, PersonMemoryStore::getPerson(11)) &&
                PersonMemoryStore::getPerson(11).is_null() && !PersonMemoryStore::getPerson(22).is_null(),
          "deletes all person data without touching others", kTestName);
        conflict = false;
        try { PersonMemoryStore::applyConsolidation(*beforeDelete,
            {{"profile", {"过期结果"}}, {"recent_interactions", json::array()}}); }
        catch (const PersonMemoryStore::ConsolidationConflict &) { conflict = true; }
        check(conflict && PersonMemoryStore::getPerson(11).is_null(),
          "daily consolidation cannot recreate deleted person", kTestName);
        MemoryMaintenanceStore::apply(db, 100, {.affinity = {{33, 1}}});
        original = PersonMemoryStore::getPerson(33);
        edited = original;
        edited["profile"] = {"首次手动记忆"};
        edited["relationship"] = {"熟悉的聊天伙伴"};
        check(PersonMemoryStore::updatePerson(33, original, edited) &&
                PersonMemoryStore::getPerson(33)["profile"] == edited["profile"],
          "affinity-only person can receive a profile", kTestName);
        check(!PersonMemoryStore::pendingSnapshot(33) && PersonMemoryStore::pendingSnapshot(33, true),
          "manual snapshot includes existing profile without pending entries", kTestName);
        const auto manual = PersonMemoryStore::pendingSnapshot(33, true);
        edited = PersonMemoryStore::getPerson(33);
        original = edited;
        edited["relationship"] = {"人工修改关系"};
        check(PersonMemoryStore::updatePerson(33, original, edited), "edits relationship", kTestName);
        conflict = false;
        try { PersonMemoryStore::applyConsolidation(*manual, original); }
        catch (const PersonMemoryStore::ConsolidationConflict &) { conflict = true; }
        check(conflict && PersonMemoryStore::getPerson(33)["relationship"] == edited["relationship"],
          "manual consolidation checks relationship even without pending entries", kTestName);
        const auto context = PersonMemoryStore::getForSenders(
          json::array({{{"sender", {{"qq", "33"}}}}}));
        check(context.size() == 1 && context[0]["relationship"] == edited["relationship"] &&
                !context[0].contains("stable_memory"), "context exposes new profile and relationship fields", kTestName);
        const AdminController controller;
        drogon::HttpResponsePtr response;
        const auto callback = [&](const drogon::HttpResponsePtr &value) { response = value; };
        const auto unchanged = PersonMemoryStore::getPerson(22);
        check(sqlite3_exec(db.handle(),
                "CREATE TRIGGER reject_unchanged_entry_update BEFORE UPDATE ON person_memory_entries "
                "BEGIN SELECT RAISE(ABORT, 'unexpected entry update'); END", nullptr, nullptr, nullptr) == SQLITE_OK,
          "creates no-op update guard", kTestName);
        check(PersonMemoryStore::updatePerson(22, unchanged, unchanged),
          "unchanged pending entries do not issue updates", kTestName);
        sqlite3_exec(db.handle(), "DROP TRIGGER reject_unchanged_entry_update", nullptr, nullptr, nullptr);
        auto duplicateEntries = unchanged;
        duplicateEntries["pending_entries"].push_back(duplicateEntries["pending_entries"][0]);
        auto duplicateRequest = drogon::HttpRequest::newHttpRequest();
        duplicateRequest->setBody(json({{"expected", unchanged}, {"replacement", duplicateEntries}}).dump());
        drogon::sync_wait(controller.updatePerson(duplicateRequest, callback, "22"));
        check(response->statusCode() == drogon::k400BadRequest && PersonMemoryStore::getPerson(22) == unchanged,
          "duplicate pending IDs are rejected without modifying data", kTestName);
        const auto update = [&](const json &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(body.dump());
            drogon::sync_wait(controller.updatePerson(request, callback, "33"));
            return response->statusCode();
        };
        original = PersonMemoryStore::getPerson(33);
        edited = original;
        for (const json &score : {json(-101), json(101), json(0.5), json("1")}) {
            edited["affinity"] = score;
            check(update({{"expected", original}, {"replacement", edited}}) == drogon::k400BadRequest,
              "API rejects invalid affinity without writing", kTestName);
        }
        edited["affinity"] = 100;
        edited["profile"] = {" "};
        check(update({{"expected", original}, {"replacement", edited}}) == drogon::k400BadRequest,
          "API rejects blank memory items", kTestName);
        edited["profile"] = json::array();
        edited["pending_entries"] = {{{"id", "999999"}, {"content", "伪造条目"}}};
        check(update({{"expected", original}, {"replacement", edited}}) == drogon::k400BadRequest,
          "API cannot add arbitrary pending IDs", kTestName);
        edited["pending_entries"] = json::array();
        check(update({{"expected", original}, {"replacement", edited}}) == drogon::k200OK &&
                PersonMemoryStore::getPerson(33)["affinity"] == 100,
          "API accepts upper affinity bound and empty categories", kTestName);
        check(update({{"expected", original}, {"replacement", edited}}) == drogon::k409Conflict,
          "API reports stale edits as HTTP 409", kTestName);
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setBody(json({{"expected", original}}).dump());
        drogon::sync_wait(controller.deletePerson(request, callback, "33"));
        check(response->statusCode() == drogon::k409Conflict, "delete API rejects stale snapshot", kTestName);
        request->setBody(json({{"expected", PersonMemoryStore::getPerson(33)}}).dump());
        drogon::sync_wait(controller.deletePerson(request, callback, "33"));
        check(response->statusCode() == drogon::k200OK && PersonMemoryStore::getPerson(33).is_null(),
          "delete API removes person", kTestName);
        db.close();
    }

    void testSessionShortTermMemory() {
        constexpr std::string_view kTestName = "session short-term memory";
        using namespace insoulforge;
        auto &db = Database::instance();
        check(db.initialize(":memory:").has_value(), "initializes database", kTestName);
        const json input = {{"aliases", {{{"qq", "11"}, {"names", {" 老王 ", "老王"}}},
          {{"qq", "11"}, {"names", {"摄影佬"}}}}}, {"recent_events", {" 联机待定 ", "联机待定"}}};
        const auto normalized = MemoryStore::normalizeSessionMemory(input);
        check(normalized && (*normalized)["aliases"].size() == 1 &&
                (*normalized)["aliases"][0]["names"] == json::array({"老王", "摄影佬"}) &&
                (*normalized)["recent_events"] == json::array({"联机待定"}),
          "deduplicates aliases by QQ, names and events", kTestName);
        auto legacyInput = input;
        legacyInput["legacy_entries"] = {" 喜欢摄影 ", "喜欢摄影", "以前的约定"};
        const auto legacyNormalized = MemoryStore::normalizeSessionMemory(legacyInput);
        check(legacyNormalized && (*legacyNormalized)["legacy_entries"] == json::array({"喜欢摄影", "以前的约定"}),
          "legacy contents remain readable and are deduplicated", kTestName);
        check(MemoryStore::normalizeSessionMemory({{"aliases", json::array()}}).has_value() &&
                MemoryStore::pendingLegacyEntries(R"(["旧条目","旧条目","第二条"])") ==
                  std::vector<std::string>{"旧条目", "第二条"} &&
                MemoryStore::pendingLegacyEntries(dumpJson(*legacyNormalized)) ==
                  std::vector<std::string>{"联机待定", "喜欢摄影", "以前的约定"},
          "accepts alias-only records and migrates old arrays and historical fields once", kTestName);
        legacyInput["legacy_entries"] = "invalid";
        check(!MemoryStore::normalizeSessionMemory(legacyInput), "rejects malformed legacy entries", kTestName);
        for (const auto &invalid: json::array({nullptr, json::array(),
               json{{"aliases", json::array()}, {"recent_events", "wrong"}},
               json{{"aliases", {{{"qq", "self"}, {"names", {"bot"}}}}}, {"recent_events", json::array()}},
               json{{"aliases", json::array()}, {"recent_events", {42}}}})) {
            check(!MemoryStore::normalizeSessionMemory(invalid), "rejects malformed structure", kTestName);
        }
        MemoryStore::updateShortTermMemory(100, " 旧事件 \n\n旧称呼\n");
        check(MemoryStore::projectShortTermMemory(MemoryStore::getShortTermMemory(100)) == json{{"aliases", json::array()}} &&
                MemoryStore::pendingLegacyEntries(MemoryStore::getShortTermMemory(100)) == std::vector<std::string>{"旧事件", "旧称呼"},
          "legacy memory remains readable without rewriting storage", kTestName);
        const AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const std::string &content) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setBody(dumpJson({{"memory", content}}));
            drogon::sync_wait(controller.updateSessionMemory(request, [&](const auto &value) { response = value; }, "100"));
        };
        save(dumpJson(input));
        check(response->getStatusCode() == drogon::k200OK &&
                MemoryStore::projectShortTermMemory(MemoryStore::getShortTermMemory(100)) == json{{"aliases", (*normalized)["aliases"]}},
          "manual editing saves normalized structured memory", kTestName);
        drogon::sync_wait(controller.getSessionMemory(drogon::HttpRequest::newHttpRequest(),
          [&](const auto &value) { response = value; }, "100"));
        const auto displayed = getStr(json::parse(response->body()), "memory");
        check(displayed.find('\n') != std::string::npos && json::parse(displayed) == *normalized,
          "existing editor receives formatted JSON", kTestName);
        check(json::parse(response->body())["conversationSummaries"] == json::array(),
          "sessions without summaries return an empty list", kTestName);
        auto withSummaries = *normalized;
        withSummaries["conversation_summaries"] = json::array({
          {{"start_time", "2026-10-29 10:00:00"}, {"end_time", "2026-10-29 11:00:00"}, {"summary", "讨论部署"}}});
        save(dumpJson(withSummaries));
        drogon::sync_wait(controller.getSessionMemory(drogon::HttpRequest::newHttpRequest(),
          [&](const auto &value) { response = value; }, "100"));
        check(json::parse(response->body())["conversationSummaries"] == withSummaries["conversation_summaries"],
          "summary viewer receives stored summaries with their time ranges", kTestName);
        save(dumpJson(input));
        for (const auto &bad: {std::string{"{"}, std::string{R"({"aliases":"invalid"})"}}) {
            save(bad);
            check(response->getStatusCode() == drogon::k400BadRequest &&
                    MemoryStore::projectShortTermMemory(MemoryStore::getShortTermMemory(100)) == json{{"aliases", (*normalized)["aliases"]}},
              "invalid manual JSON preserves saved memory", kTestName);
        }
        check(MemoryStore::getShortTermMemory(200).empty() && PersonMemoryStore::getForSenders(
                json::array({{{"sender", {{"qq", "11"}}}}})).empty() && AffinityStore::getAffinityMap().empty(),
          "session editing does not modify other sessions or global state", kTestName);
        db.close();
    }

    void testRemovedShortTermLimit() {
        constexpr std::string_view kTestName = "removed short-term limit";
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-no-memory-limit-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        {
            std::ofstream output(path);
            output << R"({"memory":{"shortTermMemoryMax":"obsolete","contextWindowLimit":73,"futureSetting":true}})";
        }
        auto &config = insoulforge::Config::instance();
        check(config.initialize(path.string()).has_value(), "loads old field without validating its obsolete type", kTestName);
        auto memory = config.getMemoryConfig();
        check(!memory.contains("shortTermMemoryMax") && memory["contextWindowLimit"] == 73 && memory["futureSetting"] == true,
          "removes obsolete field and preserves other settings", kTestName);
        {
            std::ifstream input(path);
            const auto stored = insoulforge::json::parse(input);
            check(!stored["memory"].contains("shortTermMemoryMax"), "writes migration back to disk", kTestName);
        }
        memory["shortTermMemoryMax"] = "old frontend";
        check(config.saveMemoryConfig(memory).has_value() && !config.getMemoryConfig().contains("shortTermMemoryMax") &&
                config.contextWindowLimit == 73 && config.getMemoryConfig()["futureSetting"] == true,
          "old frontend field is ignored without changing other settings", kTestName);
        check(config.initialize(path.string()).has_value() && !config.getMemoryConfig().contains("shortTermMemoryMax"),
          "removed field does not return after restart", kTestName);
        std::filesystem::remove(path);
    }

    void testImageApiConfig() {
        constexpr std::string_view kTestName = "image API configuration";
        const auto path = std::filesystem::temp_directory_path() /
            ("insoulforge-image-api-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        auto &config = insoulforge::Config::instance();
        check(config.initialize(path.string()).has_value(), "new configuration initializes", kTestName);
        auto settings = config.getLLMConfig("imageGeneration");
        check(settings["apiType"] == "openai" && settings["editPath"] == "/images/edits" &&
                config.imageGeneration.apiType == "openai", "new installations default to OpenAI", kTestName);
        settings["baseUrl"] = "https://proxy.example.com/v1";
        settings["model"] = "gpt-image-2.5-sunburst";
        settings["editPath"] = "/custom/edits";
        check(config.saveLLMConfig("imageGeneration", settings).has_value() &&
                config.imageGeneration.editPath == "/custom/edits", "edit path updates at runtime", kTestName);
        for (const auto &invalid : insoulforge::json::array({"unknown", "", 123, nullptr})) {
            auto bad = settings;
            bad["apiType"] = invalid;
            check(!config.saveLLMConfig("imageGeneration", bad) &&
                    config.getLLMConfig("imageGeneration") == settings && config.imageGeneration.apiType == "openai",
                "invalid protocol preserves configuration", kTestName);
        }
        for (const auto &invalid : insoulforge::json::array({"", " \t", 123, nullptr})) {
            auto bad = settings;
            bad["editPath"] = invalid;
            check(!config.saveLLMConfig("imageGeneration", bad) && config.getLLMConfig("imageGeneration") == settings,
                "invalid edit path preserves configuration", kTestName);
        }
        auto oldClient = settings;
        oldClient.erase("apiType");
        oldClient.erase("editPath");
        check(config.saveLLMConfig("imageGeneration", oldClient).has_value() &&
                nlohmann::json::parse(config.getLLMConfig("imageGeneration").dump()) ==
                  nlohmann::json::parse(settings.dump()),
            "older clients preserve protocol and edit path", kTestName);
        settings["apiType"] = "openrouter";
        check(config.saveLLMConfig("imageGeneration", settings).has_value() &&
                config.imageGeneration.apiType == "openrouter", "protocol switches without restart", kTestName);
        check(config.initialize(path.string()).has_value() && config.imageGeneration.apiType == "openrouter" &&
                config.imageGeneration.editPath == "/custom/edits", "protocol persists after restart", kTestName);
        {
            std::ofstream output(path);
            output << R"({"llm":{"imageGeneration":{"apiKey":"old-key","baseUrl":"https://old.example.com","path":"/custom/images","model":"old-model"}}})";
        }
        check(config.initialize(path.string()).has_value() && config.imageGeneration.apiType == "openrouter" &&
                config.imageGeneration.path == "/custom/images" && config.imageGeneration.apiKey == "old-key" &&
                config.getLLMConfig("imageGeneration")["apiType"] == "openrouter",
            "legacy image settings retain their original protocol and connection", kTestName);
        std::filesystem::remove(path);
    }

    void testAffinitySwitch() {
        using namespace insoulforge;
        constexpr std::string_view kTestName = "affinity switch";
        auto &config = Config::instance();
        auto &database = Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        config.affinityEnabled = true;
        MemoryMaintenanceStore::apply(database, 100, {.personEntries = {{11, "喜欢摄影"}}, .affinity = {{11, 7}}});
        config.affinityEnabled = false;
        MemoryMaintenanceStore::apply(database, 100, {.personEntries = {{11, "会修相机"}}, .affinity = {{11, 5}, {22, 3}}});
        AffinityStore::adjustAffinity(11, 5);
        check(AffinityStore::getAffinityMap().empty(), "disabled scores cannot be read", kTestName);
        auto person = PersonMemoryStore::getPerson(11);
        check(!person.contains("affinity") && person["pending_entries"].size() == 2 &&
                !PersonMemoryStore::listPeople()[0].contains("affinity"),
          "disabled score is hidden while memory still updates", kTestName);
        auto replacement = person;
        replacement["profile"] = {"喜欢摄影，会修相机"};
        replacement["affinity"] = 99;
        check(PersonMemoryStore::updatePerson(11, person, replacement), "disabled score is ignored when editing", kTestName);
        person = PersonMemoryStore::getPerson(11);
        AdminController controller;
        drogon::HttpResponsePtr response;
        const auto callback = [&](const auto &value) { response = value; };
        drogon::sync_wait(controller.getSessionAffinity(drogon::HttpRequest::newHttpRequest(), callback, "100"));
        check(response->statusCode() == drogon::k403Forbidden, "legacy score API is disabled", kTestName);
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        request->setBody(json{{"expected", person}, {"replacement", person}}.dump());
        drogon::sync_wait(controller.updatePerson(request, callback, "11"));
        check(response->statusCode() == drogon::k200OK, "memory API accepts edits without scores", kTestName);
        config.affinityEnabled = true;
        check(AffinityStore::getAffinityMap().at(11) == 7 && !AffinityStore::getAffinityMap().contains(22) &&
                PersonMemoryStore::getPerson(11)["profile"] == replacement["profile"],
          "re-enabling restores historical scores without backfilling", kTestName);
        MemoryMaintenanceStore::apply(database, 100, {.affinity = {{11, 2}}});
        check(AffinityStore::getAffinityMap().at(11) == 9, "updates resume after re-enabling", kTestName);
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
        auto &config = insoulforge::Config::instance();
        check(config.affinityEnabled && config.getMemoryConfig()["affinityEnabled"] == true,
          "old configurations enable affinity by default", kTestName);
        check(config.saveMemoryConfig({{"affinityEnabled", false}}).has_value() && !config.affinityEnabled,
          "affinity switch hot updates", kTestName);
        check(config.saveMemoryConfig({{"sharedConversationMessageCount", 10}}).has_value() && !config.affinityEnabled,
          "old client preserves affinity switch", kTestName);
        for (const auto &invalid: insoulforge::json::array({0, 1, "false", nullptr}))
            check(!config.saveMemoryConfig({{"affinityEnabled", invalid}}) && !config.affinityEnabled,
              "affinity switch rejects non-boolean values", kTestName);
        check(config.initialize(path.string()).has_value() && !config.affinityEnabled,
          "affinity switch persists", kTestName);
        check(config.saveMemoryConfig({{"affinityEnabled", true}}).has_value() && config.affinityEnabled,
          "affinity can be re-enabled", kTestName);
        check(config.sharedConversationMessageCount == 10 && config.getMemoryConfig()["sharedConversationMessageCount"] == 10,
          "old configuration receives ten shared messages per session", kTestName);
        check(config.saveMemoryConfig({{"sharedConversationMessageCount", 25}}).has_value() &&
                config.sharedConversationMessageCount == 25,
          "sharing count updates at runtime", kTestName);
        for (const auto &invalid: insoulforge::json::array({0, -1, 1.5, "10", true, nullptr})) {
            check(!config.saveMemoryConfig({{"sharedConversationMessageCount", invalid}}) &&
                    config.sharedConversationMessageCount == 25,
              "invalid counts preserve configuration", kTestName);
        }
        check(config.initialize(path.string()).has_value() && config.sharedConversationMessageCount == 25,
          "sharing count persists", kTestName);
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
            if (const auto &function = tool["function"]; function["name"] == "create_scheduled_task") {
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
                  request, [&rejected](const drogon::HttpResponsePtr &response) -> void { rejected = response; },
                  [&continued] -> void { continued = true; });
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
        check(!insoulforge::Config::instance().defaultPrivateEnabled.load() &&
                insoulforge::Config::instance().getQQConfig()["defaultPrivateEnabled"] == false,
          "new private sessions default to disabled", kTestName);
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
        check(config
                  .saveQQConfig(
                    {{"botName", "facade-bot"}, {"selfQQNumber", 3457355246ULL}, {"defaultPrivateEnabled", true}})
                  .has_value() &&
                config.botName == "facade-bot" && config.selfQQNumber == 3457355246ULL &&
                config.defaultPrivateEnabled.load() && config.getQQConfig()["defaultPrivateEnabled"] == true,
          "saving immediately applies typed QQ configuration", kTestName);
        auto stringQQ = config.getQQConfig();
        stringQQ["selfQQNumber"] = "9007199254740993";
        check(config.saveQQConfig(stringQQ).has_value() && config.selfQQNumber == 9007199254740993ULL &&
                config.getQQConfig()["selfQQNumber"] == 9007199254740993ULL,
          "decimal QQ strings retain precision in storage and runtime", kTestName);
        stringQQ["selfQQNumber"] = 3457355246ULL;
        check(config.saveQQConfig(stringQQ).has_value(), "numeric QQ configuration remains compatible", kTestName);
        const auto oldQQ = config.getQQConfig();
        for (const auto &invalid:
          insoulforge::json::array({{{"selfQQNumber", -1}}, {{"selfQQNumber", 123.5}}, {{"selfQQNumber", ""}},
            {{"selfQQNumber", "12x"}}, {{"selfQQNumber", "-1"}}, {{"selfQQNumber", "18446744073709551616"}},
            {{"botName", 23}}, {{"defaultPrivateEnabled", "true"}}, {{"defaultPrivateEnabled", 1}}})) {
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
            const std::string content{std::istreambuf_iterator(input), std::istreambuf_iterator<char>()};
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

    void testGroupCardSettings() {
        constexpr std::string_view kTestName = "group card settings";
        auto &db = insoulforge::Database::instance();
        check(db.initialize(":memory:").has_value(), "initializes database", kTestName);
        check(!insoulforge::SessionStore::getSessionConfig(100).sendGroupCard, "defaults to disabled", kTestName);
        insoulforge::SessionStore::incrementMessageCount(100);
        insoulforge::SessionStore::setSendGroupCard(100, true);
        insoulforge::SessionStore::incrementMessageCount(100);
        check(insoulforge::SessionStore::getSessionConfig(100).sendGroupCard &&
                insoulforge::SessionStore::getSessionConfig(100).allMesCount == 2 &&
                !insoulforge::SessionStore::getSessionConfig(200).sendGroupCard,
          "groups are independent and statistics preserved", kTestName);
        insoulforge::SessionStore::enableSession(100);
        insoulforge::SessionStore::disableSession(100);
        insoulforge::SessionStore::enableSession(100);
        check(
          insoulforge::SessionStore::getSessionConfig(100).sendGroupCard, "enable cycle preserves setting", kTestName);
        auto command = insoulforge::json{{"session_id", 100}, {"sender", {{"qq", "11"}}},
          {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "/groupcard off"}}})}};
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(
          insoulforge::SessionStore::getSessionConfig(100).sendGroupCard, "non-admin cannot change setting", kTestName);
        insoulforge::AdminStore::addAdmin(11);
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(
          !insoulforge::SessionStore::getSessionConfig(100).sendGroupCard, "admin command disables setting", kTestName);
        command["segments"][0]["text"] = "/groupcard on";
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(
          insoulforge::SessionStore::getSessionConfig(100).sendGroupCard, "admin command enables setting", kTestName);
        command["session_id"] = insoulforge::SessionId::fromPrivateUser(11);
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(!insoulforge::SessionStore::hasSessionConfig(insoulforge::SessionId::fromPrivateUser(11)),
          "private command rejected", kTestName);
        // Simulate the last schema before the group-card column was introduced.
        check(sqlite3_exec(db.handle(), "ALTER TABLE group_config DROP COLUMN send_group_card", nullptr, nullptr,
                nullptr) == SQLITE_OK,
          "restores old schema", kTestName);
        check(sqlite3_exec(db.handle(), "PRAGMA user_version = 14", nullptr, nullptr, nullptr) == SQLITE_OK,
          "sets old schema version", kTestName);
        insoulforge::SchemaMigrator::migrate(db.handle());
        check(!insoulforge::SessionStore::getSessionConfig(100).sendGroupCard &&
                insoulforge::SessionStore::getSessionConfig(100).allMesCount == 2,
          "migration defaults off and preserves statistics", kTestName);
        db.close();
    }

    void testGroupAutoReply() {
        constexpr std::string_view kTestName = "group auto reply";
        auto &db = insoulforge::Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-auto-reply-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        check(insoulforge::SessionStore::getSessionConfig(100).autoReply, "new groups default on", kTestName);
        insoulforge::SessionStore::incrementMessageCount(100);
        insoulforge::SessionStore::setSendGroupCard(100, true);
        insoulforge::SessionStore::setAutoReply(100, false);
        insoulforge::SessionStore::incrementMessageCount(100);
        check(!insoulforge::SessionStore::getSessionConfig(100).autoReply &&
                insoulforge::SessionStore::getSessionConfig(200).autoReply &&
                insoulforge::SessionStore::getSessionConfig(100).sendGroupCard &&
                insoulforge::SessionStore::getSessionConfig(100).allMesCount == 2,
          "settings are independent and statistics preserved", kTestName);
        insoulforge::SessionStore::disableSession(100);
        insoulforge::SessionStore::enableSession(100);
        check(!insoulforge::SessionStore::getSessionConfig(100).autoReply, "enable cycle preserves setting", kTestName);

        auto &config = insoulforge::Config::instance();
        const auto originalSelf = config.selfQQNumber;
        const auto originalRouter = config.router;
        config.selfQQNumber = 42;
        config.router = {};
        auto message = insoulforge::json{{"message_id", "auto-test"}, {"sender", {{"qq", "11"}}},
          {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "ordinary group message"}}})}};
        const auto route = [&]() {
            return drogon::sync_wait(
              insoulforge::MessageRouter::route(100, "auto-test", insoulforge::json::array({message})));
        };
        check(route().action == insoulforge::RouterDecision::Action::SKIP, "normal messages skip without model request",
          kTestName);
        message["segments"].push_back({{"type", "at"}, {"target", {{"qq", "99"}}}});
        check(route().action == insoulforge::RouterDecision::Action::SKIP, "mentioning another user still skips",
          kTestName);
        message["segments"].back()["target"]["qq"] = "42";
        check(route().action == insoulforge::RouterDecision::Action::REPLY, "bot mention still replies", kTestName);
        message["segments"].erase(1);
        message["sender"]["qq"] = std::to_string(insoulforge::SessionId::kSystemAccountId);
        check(route().action == insoulforge::RouterDecision::Action::REPLY, "system tasks still reply", kTestName);
        message["sender"]["qq"] = "11";
        const auto privateId = insoulforge::SessionId::fromPrivateUser(11);
        insoulforge::SessionStore::setAutoReply(privateId, false);
        check(drogon::sync_wait(
                insoulforge::MessageRouter::route(privateId, "auto-test", insoulforge::json::array({message})))
                  .action == insoulforge::RouterDecision::Action::REPLY,
          "private replies are unaffected", kTestName);
        config.selfQQNumber = originalSelf;
        config.router = originalRouter;

        auto command = insoulforge::json{{"session_id", 100}, {"sender", {{"qq", "11"}}},
          {"segments", insoulforge::json::array({{{"type", "text"}, {"text", "/autoreply on"}}})}};
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(
          !insoulforge::SessionStore::getSessionConfig(100).autoReply, "non-admin cannot change setting", kTestName);
        insoulforge::AdminStore::addAdmin(11);
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(insoulforge::SessionStore::getSessionConfig(100).autoReply, "admin command enables setting", kTestName);
        command["segments"][0]["text"] = "/autoreply off";
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(!insoulforge::SessionStore::getSessionConfig(100).autoReply, "admin command disables setting", kTestName);
        command["segments"][0]["text"] = "/autoreply invalid";
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(
          !insoulforge::SessionStore::getSessionConfig(100).autoReply, "invalid argument preserves setting", kTestName);
        command["session_id"] = privateId;
        command["segments"][0]["text"] = "/autoreply on";
        drogon::sync_wait(insoulforge::CommandProcessor::execute(command));
        check(!insoulforge::SessionStore::getSessionConfig(privateId).autoReply, "private command rejected", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const std::string &id, const insoulforge::json &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(insoulforge::dumpJson(body));
            drogon::sync_wait(controller.saveGroupAutoReply(request, [&](const auto &value) { response = value; }, id));
        };
        save("100", {{"autoReply", true}});
        check(response->getStatusCode() == drogon::k200OK && insoulforge::SessionStore::getSessionConfig(100).autoReply,
          "web endpoint enables setting", kTestName);
        save("100", {{"autoReply", false}});
        check(
          response->getStatusCode() == drogon::k200OK && !insoulforge::SessionStore::getSessionConfig(100).autoReply,
          "web endpoint disables setting", kTestName);
        save("100", {{"autoReply", "false"}});
        check(response->getStatusCode() == drogon::k400BadRequest, "rejects non-boolean value", kTestName);
        save(std::to_string(privateId), {{"autoReply", true}});
        check(response->getStatusCode() == drogon::k400BadRequest, "web endpoint rejects private session", kTestName);
        save("0", {{"autoReply", true}});
        check(response->getStatusCode() == drogon::k400BadRequest, "web endpoint rejects invalid group", kTestName);

        db.close();
        check(db.initialize(path.string()).has_value() && !insoulforge::SessionStore::getSessionConfig(100).autoReply,
          "setting survives restart", kTestName);
        check(sqlite3_exec(db.handle(), "ALTER TABLE group_config DROP COLUMN auto_reply; PRAGMA user_version = 15",
                nullptr, nullptr, nullptr) == SQLITE_OK,
          "restores v15 schema", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value() && insoulforge::SessionStore::getSessionConfig(100).autoReply &&
                insoulforge::SessionStore::getSessionConfig(100).sendGroupCard &&
                insoulforge::SessionStore::getSessionConfig(100).allMesCount == 2,
          "v15 upgrade defaults on and preserves other fields", kTestName);
        db.close();
        std::filesystem::remove(path);
    }

    void testGroupCommandSettings() {
        constexpr std::string_view kTestName = "group command settings";
        auto &db = insoulforge::Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-command-settings-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        check(insoulforge::SessionStore::getSessionConfig(100).commandEnabled, "defaults on", kTestName);
        insoulforge::SessionStore::setSendGroupCard(100, true);
        insoulforge::SessionStore::setAutoReply(100, false);
        insoulforge::SessionStore::setAdditionalPrompt(100, "test prompt");
        insoulforge::SessionStore::incrementMessageCount(100);
        insoulforge::SessionStore::setCommandEnabled(100, false);
        auto saved = insoulforge::SessionStore::getSessionConfig(100);
        check(!saved.commandEnabled && saved.sendGroupCard && !saved.autoReply && saved.allMesCount == 1 &&
                saved.additionalPrompt == "test prompt",
          "single-field update preserves other settings", kTestName);
        insoulforge::SessionStore::saveSessionConfig(100, saved);
        insoulforge::SessionStore::disableSession(100);
        insoulforge::SessionStore::enableSession(100);
        check(!insoulforge::SessionStore::getSessionConfig(100).commandEnabled,
          "full save and enable cycle preserve setting", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const std::string &id, const insoulforge::json &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(insoulforge::dumpJson(body));
            drogon::sync_wait(
              controller.saveGroupCommandEnabled(request, [&](const auto &value) { response = value; }, id));
        };
        save("100", {{"commandEnabled", true}});
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::SessionStore::getSessionConfig(100).commandEnabled,
          "web endpoint enables setting", kTestName);
        save("100", {{"commandEnabled", false}});
        check(response->getStatusCode() == drogon::k200OK &&
                !insoulforge::SessionStore::getSessionConfig(100).commandEnabled,
          "web endpoint disables setting", kTestName);
        for (const auto &id:
          {std::string{"0"}, std::string{"invalid"}, std::to_string(insoulforge::SessionId::fromPrivateUser(11))}) {
            save(id, {{"commandEnabled", true}});
            check(response->getStatusCode() == drogon::k400BadRequest, "rejects invalid group", kTestName);
        }
        save("100", {{"commandEnabled", "false"}});
        check(response->getStatusCode() == drogon::k400BadRequest, "rejects non-boolean setting", kTestName);
        db.close();
        check(
          db.initialize(path.string()).has_value() && !insoulforge::SessionStore::getSessionConfig(100).commandEnabled,
          "setting survives restart", kTestName);
        check(
          sqlite3_exec(db.handle(), "ALTER TABLE group_config DROP COLUMN command_enabled; PRAGMA user_version = 18",
            nullptr, nullptr, nullptr) == SQLITE_OK,
          "restores v18 schema", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value(), "upgrades v18 schema", kTestName);
        saved = insoulforge::SessionStore::getSessionConfig(100);
        check(saved.commandEnabled && saved.sendGroupCard && !saved.autoReply && saved.allMesCount == 1 &&
                saved.additionalPrompt == "test prompt",
          "migration defaults on and preserves existing data", kTestName);
        db.close();
        std::filesystem::remove(path);
    }

    void testCrossSessionSharing() {
        constexpr std::string_view kTestName = "cross session sharing";
        using insoulforge::json;
        auto &db = insoulforge::Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-sharing-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        const auto privateId = insoulforge::SessionId::fromPrivateUser(11);
        for (const auto id: {u64{100}, u64{200}, u64{300}, privateId}) {
            insoulforge::SessionStore::enableSession(id);
            check(!insoulforge::SessionStore::getSessionConfig(id).crossSessionSharing,
              "groups and private sessions default off", kTestName);
        }
        insoulforge::SessionStore::setCrossSessionSharing(200, true);
        check(insoulforge::SessionStore::getSharedSessions(100).empty(), "nonparticipant receives nothing", kTestName);
        insoulforge::SessionStore::setCrossSessionSharing(100, true);
        insoulforge::SessionStore::setCrossSessionSharing(privateId, true);
        insoulforge::SessionStore::updateSessionName(200, "shared group");
        auto sources = insoulforge::SessionStore::getSharedSessions(100);
        check(sources.size() == 2 &&
                std::ranges::find(sources, std::pair<u64, std::string>{200, "shared group"}) != sources.end() &&
                std::ranges::any_of(sources, [privateId](const auto &source) { return source.first == privateId; }),
          "excludes self and nonparticipants, includes private sources", kTestName);
        check(insoulforge::SessionStore::getSharedSessions(privateId).size() == 2,
          "private participants also receive group context", kTestName);
        insoulforge::SessionStore::toggleSessionStatus(200);
        check(insoulforge::SessionStore::getSharedSessions(100).size() == 1 &&
                insoulforge::SessionStore::getSharedSessions(200).empty(),
          "disabled sessions neither send nor receive", kTestName);
        insoulforge::SessionStore::setSendGroupCard(100, true);
        insoulforge::SessionStore::incrementMessageCount(100);
        auto config = insoulforge::SessionStore::getSessionConfig(100);
        insoulforge::SessionStore::saveSessionConfig(100, config);
        check(insoulforge::SessionStore::getSessionConfig(100).crossSessionSharing && config.allMesCount == 1,
          "full saves and statistics preserve sharing", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const std::string &id, const json &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(insoulforge::dumpJson(body));
            drogon::sync_wait(controller.saveSessionSharing(request, [&](const auto &value) { response = value; }, id));
        };
        save(std::to_string(privateId), {{"crossSessionSharing", false}});
        check(response->getStatusCode() == drogon::k200OK &&
                !insoulforge::SessionStore::getSessionConfig(privateId).crossSessionSharing,
          "private sharing can be disabled through API", kTestName);
        save("100", {{"crossSessionSharing", "true"}});
        check(response->getStatusCode() == drogon::k400BadRequest &&
                insoulforge::SessionStore::getSessionConfig(100).crossSessionSharing,
          "rejects non-boolean without changing state", kTestName);
        save("0", {{"crossSessionSharing", true}});
        check(response->getStatusCode() == drogon::k400BadRequest, "rejects invalid session", kTestName);

        insoulforge::MessageList list(100, db, insoulforge::Config::instance());
        const auto message = [](const std::string &time, const std::string &text) {
            return json{{"time", time}, {"sender", {{"name", "test"}, {"qq", "11"}}},
              {"message_id", "123"}, {"reply_to", {{"message_id", "122"}}},
              {"segments", json::array({{{"type", "text"}, {"text", text}}})}};
        };
        const auto append = [&](json record) {
            record.erase("message_id");
            static_cast<void>(list.append(std::move(record)));
        };
        append(message("2026-10-15 12:00:00", "expired"));
        append(message("2026-10-16 12:00:00", "boundary"));
        append(message("2026-10-18 12:00:00", "recent"));
        append(message("2026-10-20 12:00:00", "future"));
        append(message("", "missing time"));
        auto failed = message("2026-10-18 13:00:00", "not delivered");
        failed["send_error"] = "failed";
        append(failed);
        auto internal = message("2026-10-18 14:00:00", "internal");
        internal["record_type"] = "tool_execution";
        append(internal);
        const auto snapshot = list.sharedSnapshot(10, "2026-10-16 12:00:00", "2026-10-19 12:00:00");
        check(snapshot.size() == 2 && snapshot[0]["segments"][0]["text"] == "boundary" &&
                snapshot[1]["segments"][0]["text"] == "recent",
          "time window includes boundary, excludes future and internal messages, preserves order", kTestName);
        check(!snapshot[0].contains("message_id") && !snapshot[0].contains("reply_to"),
          "shared messages cannot be referenced or recalled", kTestName);
        check(list.sharedSnapshot(1, "2026-10-16 12:00:00", "2026-10-19 12:00:00").size() == 1 &&
                list.fullSnapshot().size() == 7,
          "sharing limit is independent and does not alter history", kTestName);
        std::string chinese;
        for (int index = 0; index < 120; ++index) chinese += "中";
        auto longMessage = message("2026-10-18 12:00:00", chinese);
        longMessage["segments"].push_back({{"type", "image"}, {"recognition_status", "succeeded"},
          {"description", chinese}, {"image_index", 0}});
        longMessage["memories"] = {"not shared"};
        longMessage["tool_history"] = {"not shared"};
        const auto projected = insoulforge::SharedConversationContext::projectMessage(longMessage);
        check(projected && (*projected)["truncated"] == true &&
                (*projected)["segments"][0]["text"].get<std::string>().size() == 120 * 3 &&
                (*projected)["segments"][1]["description"].get<std::string>().size() == 80 * 3 &&
                !(*projected)["segments"][1].contains("image_index") && !projected->contains("memories") &&
                !projected->contains("tool_history"),
          "text and image descriptions share a 200-codepoint budget without media indices or recalled memories", kTestName);
        auto exact = message("2026-10-18 12:00:00", std::string(199, 'a') + "😀");
        exact["sender"]["card"] = "not shared";
        const auto exactProjection = insoulforge::SharedConversationContext::projectMessage(exact);
        check(exactProjection && !exactProjection->contains("truncated") &&
                !(*exactProjection)["sender"].contains("card"),
          "exactly 200 codepoints are intact and group cards are not leaked", kTestName);
        exact["segments"][0]["text"] = std::string(199, 'a') + "😀尾";
        const auto cut = insoulforge::SharedConversationContext::projectMessage(exact);
        check(cut && (*cut)["truncated"] == true &&
                (*cut)["segments"][0]["text"] == std::string(199, 'a') + "😀",
          "truncation preserves four-byte Unicode characters", kTestName);
        exact["sender"]["qq"] = std::to_string(insoulforge::SessionId::kSystemAccountId);
        check(!insoulforge::SharedConversationContext::projectMessage(exact),
          "internal system messages are not shared", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value() &&
                insoulforge::SessionStore::getSessionConfig(100).crossSessionSharing,
          "sharing survives restart", kTestName);
        check(sqlite3_exec(db.handle(),
                "ALTER TABLE group_config DROP COLUMN cross_session_sharing; PRAGMA user_version = 19",
                nullptr, nullptr, nullptr) == SQLITE_OK,
          "restores v19 schema", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value(), "migrates v19", kTestName);
        config = insoulforge::SessionStore::getSessionConfig(100);
        check(!config.crossSessionSharing && config.sendGroupCard && config.allMesCount == 1,
          "migration defaults sharing off and preserves other settings", kTestName);
        db.close();
        std::filesystem::remove(path);
    }

    void testGroupAdditionalPrompt() {
        constexpr std::string_view kTestName = "group additional prompt";
        auto &db = insoulforge::Database::instance();
        const auto path = std::filesystem::temp_directory_path() /
                          ("insoulforge-group-prompt-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        check(db.initialize(path.string()).has_value(), "initializes database", kTestName);
        insoulforge::PromptService::initialize(db);
        const auto base = insoulforge::ExecutorAgent::buildSystemPrompt(100);
        check(insoulforge::SessionStore::getSessionConfig(100).additionalPrompt.empty(), "defaults empty", kTestName);
        const std::string prompt = "本群回复使用中文。\n保留专业名词。";
        insoulforge::SessionStore::setAdditionalPrompt(100, prompt);
        insoulforge::SessionStore::setSendGroupCard(100, true);
        insoulforge::SessionStore::setAutoReply(100, false);
        insoulforge::SessionStore::incrementMessageCount(100);
        const auto combined = insoulforge::ExecutorAgent::buildSystemPrompt(100);
        check(combined.starts_with(base) && combined.ends_with(prompt), "appends without replacing global prompt",
          kTestName);
        check(insoulforge::ExecutorAgent::buildSystemPrompt(200) == base,
          "other groups do not inherit prompt", kTestName);
        const auto privateId = insoulforge::SessionId::fromPrivateUser(11);
        insoulforge::SessionStore::setAdditionalPrompt(privateId, "private-marker");
        check(insoulforge::ExecutorAgent::buildSystemPrompt(privateId).find("private-marker") ==
                std::string::npos,
          "private session does not receive group prompt", kTestName);
        insoulforge::SessionStore::disableSession(100);
        insoulforge::SessionStore::enableSession(100);
        check(insoulforge::SessionStore::getSessionConfig(100).additionalPrompt == prompt &&
                !insoulforge::SessionStore::getSessionConfig(100).autoReply &&
                insoulforge::SessionStore::getSessionConfig(100).sendGroupCard,
          "other operations preserve prompt", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto save = [&](const std::string &id, const insoulforge::json &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(insoulforge::dumpJson(body));
            drogon::sync_wait(controller.saveGroupPrompt(request, [&](const auto &value) { response = value; }, id));
        };
        save("100", {{"additionalPrompt", prompt}});
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::SessionStore::getSessionConfig(100).additionalPrompt == prompt,
          "endpoint saves prompt", kTestName);
        drogon::sync_wait(controller.getGroupPrompt(
          drogon::HttpRequest::newHttpRequest(), [&](const auto &value) { response = value; }, "100"));
        check(insoulforge::parseJson(std::string(response->body()))["additionalPrompt"] == prompt,
          "endpoint loads prompt", kTestName);
        save("100", {{"additionalPrompt", 123}});
        check(response->getStatusCode() == drogon::k400BadRequest, "rejects non-string", kTestName);
        save(std::to_string(privateId), {{"additionalPrompt", "disallowed"}});
        check(response->getStatusCode() == drogon::k400BadRequest, "endpoint rejects private session", kTestName);
        save("100", {{"additionalPrompt", std::string(insoulforge::SessionStore::kMaxAdditionalPromptBytes + 1, 'a')}});
        check(response->getStatusCode() == drogon::k400BadRequest &&
                insoulforge::SessionStore::getSessionConfig(100).additionalPrompt == prompt,
          "oversized prompt preserves old setting", kTestName);
        save("100", {{"additionalPrompt", ""}});
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::SessionStore::getSessionConfig(100).additionalPrompt.empty(),
          "endpoint clears prompt", kTestName);
        insoulforge::SessionStore::setAdditionalPrompt(100, prompt);
        db.close();
        check(db.initialize(path.string()).has_value() &&
                insoulforge::SessionStore::getSessionConfig(100).additionalPrompt == prompt,
          "prompt survives restart", kTestName);
        check(
          sqlite3_exec(db.handle(), "ALTER TABLE group_config DROP COLUMN additional_prompt; PRAGMA user_version = 16",
            nullptr, nullptr, nullptr) == SQLITE_OK,
          "restores v16 schema", kTestName);
        db.close();
        check(db.initialize(path.string()).has_value() &&
                insoulforge::SessionStore::getSessionConfig(100).additionalPrompt.empty() &&
                !insoulforge::SessionStore::getSessionConfig(100).autoReply &&
                insoulforge::SessionStore::getSessionConfig(100).sendGroupCard &&
                insoulforge::SessionStore::getSessionConfig(100).allMesCount == 1,
          "migration adds empty prompt and preserves existing settings", kTestName);
        db.close();
        std::filesystem::remove(path);
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
        check(insoulforge::Config::instance().getMcpConfig()["servers"].is_array() &&
                insoulforge::Config::instance().getMcpConfig()["servers"].empty(),
          "old files receive disabled MCP configuration", kTestName);
        check(insoulforge::Config::instance().execution.maxToolRounds.load() == 8, "loads the default runtime limit",
          kTestName);
        check(insoulforge::Config::instance().getExecutionConfig()["mcpToolPageSize"] == 10 &&
                insoulforge::Config::instance().getExecutionConfig()["mcpMaxToolsPerEnable"] == 4 &&
                insoulforge::Config::instance().getExecutionConfig()["mcpMaxToolsPerReply"] == 12,
          "old files receive MCP defaults", kTestName);
        check(
          insoulforge::Config::instance().executorFallback.model.empty(), "fallback is disabled by default", kTestName);
        check(insoulforge::Config::instance()
                .saveExecutionConfig({{"maxToolRounds", 8}, {"futureSetting", true}})
                .has_value(),
          "saves execution config", kTestName);

        const insoulforge::AdminController controller;
        drogon::HttpResponsePtr response;
        const auto callback = [&response](const drogon::HttpResponsePtr &value) -> void { response = value; };
        drogon::sync_wait(controller.getExecutionConfig(drogon::HttpRequest::newHttpRequest(), callback));
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::parseJson(std::string(response->body()))["maxToolRounds"] == 8,
          "GET returns stored execution settings", kTestName);

        const auto save = [&](const std::string &body) -> void {
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

        save(R"({"maxToolRounds":100,"mcpToolPageSize":50,"mcpMaxToolsPerEnable":20,"mcpMaxToolsPerReply":64})");
        check(response->getStatusCode() == drogon::k200OK &&
                insoulforge::Config::instance().getExecutionConfig()["mcpMaxToolsPerReply"] == 64,
          "MCP boundaries save", kTestName);
        save(R"({"maxToolRounds":100,"mcpToolPageSize":10,"mcpMaxToolsPerEnable":4,"mcpMaxToolsPerReply":12})");
        const auto stored = insoulforge::Config::instance().getExecutionConfig();
        const auto invalidMcpValues = insoulforge::json{{"mcpToolPageSize", {0, 51, 1.5, "10"}},
          {"mcpMaxToolsPerEnable", {0, 21, 13}}, {"mcpMaxToolsPerReply", {0, 65, 3}}};
        for (const auto &[field, badValues]: invalidMcpValues.items()) {
            for (const auto &value: badValues) {
                save(insoulforge::dumpJson({{"maxToolRounds", 100}, {field, value}}));
                check(response->getStatusCode() == drogon::k400BadRequest &&
                        insoulforge::Config::instance().getExecutionConfig() == stored,
                  "invalid MCP limits preserve configuration", kTestName);
            }
        }
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
        {
            std::ofstream output(path);
            output
              << R"({"execution":{"maxToolRounds":8,"mcpToolPageSize":0,"mcpMaxToolsPerEnable":20,"mcpMaxToolsPerReply":1}})";
        }
        check(insoulforge::Config::instance().initialize(path.string()).has_value(), "repairs MCP limits", kTestName);
        check(insoulforge::Config::instance().getExecutionConfig()["mcpToolPageSize"] == 10 &&
                insoulforge::Config::instance().getExecutionConfig()["mcpMaxToolsPerEnable"] == 4 &&
                insoulforge::Config::instance().getExecutionConfig()["mcpMaxToolsPerReply"] == 12,
          "startup restores invalid and contradictory MCP limits", kTestName);
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
        check(!insoulforge::MessageRecord::projectForAgent(sent, true).contains("send_error"),
          "successful messages do not have a send error", kTestName);
        auto failed = insoulforge::MessageRecord::createAssistantRecord("机器人(我)", std::nullopt, "最终回复");
        failed["send_error"] = "retcode=1200, message=机器人已被禁言";
        const auto failedProjection = insoulforge::MessageRecord::projectForAgent(failed);
        check(failedProjection["send_error"] == failed["send_error"] && !failedProjection.contains("message_id") &&
                insoulforge::MessageRecord::extractText(failed) == "最终回复",
          "failed delivery preserves attempted content and error without a platform message ID", kTestName);
        const auto failedImage =
          insoulforge::MessageRecord::createAssistantGeneratedImageRecord("机器人(我)", std::nullopt, "生成的图片描述");
        check(!failedImage.contains("message_id") && failedImage["segments"][0]["description"] == "生成的图片描述",
          "generated image records support missing platform message IDs", kTestName);
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
        check(update && update->back()["tool_history"] == history, "no_reply history enters the context snapshot",
          kTestName);
        const auto failedUpdate = messages.append(failed);
        check(failedUpdate && failedUpdate->back()["send_error"] == failed["send_error"],
          "failed delivery enters the context snapshot", kTestName);
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
        const auto primaryExecutor = insoulforge::Config::instance().getLLMConfig("executor");
        check(insoulforge::Config::instance()
                .saveLLMConfig(
                  "executorFallback", {{"apiKey", "fallback-key"}, {"baseUrl", "https://fallback.example.com/v1"},
                                        {"path", "/chat/completions"}, {"model", "fallback-model"}, {"maxTokens", 300},
                                        {"temperature", 0.5}, {"topP", 0.8}, {"reasoningEffort", "none"}})
                .has_value(),
          "saves executor fallback config", kTestName);
        check(insoulforge::Config::instance().executorFallback.model == "fallback-model" &&
                insoulforge::Config::instance().executorFallbackParams.maxTokens == 300,
          "loads independent fallback model and parameters", kTestName);
        check(insoulforge::Config::instance().getLLMConfig("executor") == primaryExecutor,
          "fallback configuration preserves primary executor", kTestName);
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

    void testAgentMessageSnapshotOwnership() {
        constexpr std::string_view kTestName = "agent message snapshot ownership";
        using insoulforge::ChatRecordManager;
        using insoulforge::json;
        static_assert(std::is_same_v<decltype(std::declval<const ChatRecordManager &>().getRecords()), const json &>);
        auto messages = json::array({"legacy text",
          {{"message_id", "1"}, {"sender", {{"qq", "11"}}},
            {"segments", {{{"type", "text"}, {"text", "original"}}}}}});
        const auto expected = messages;
        const ChatRecordManager copied(100, messages);
        messages[1]["segments"][0]["text"] = "changed";
        messages.push_back({{"message_id", "2"}});
        check(copied.getSessionId() == 100 && copied.getRecords() == expected,
          "snapshot owns an independent ordered copy including plain text", kTestName);
        check(&copied.getRecords() == &copied.getRecords(), "repeated reads refer to the owned snapshot", kTestName);
        const auto *ownedElements = &messages[0];
        const ChatRecordManager moved(200, std::move(messages));
        check(&moved.getRecords()[0] == ownedElements, "moving transfers the array without copying its messages",
          kTestName);
        check(copied.getRecords() == expected, "later snapshots do not change the frozen context", kTestName);
    }

    void testMessageSnapshotAtomicReplacement() {
        constexpr std::string_view kTestName = "atomic message snapshot replacement";
        auto &database = insoulforge::Database::instance();
        check(database.initialize(":memory:").has_value(), "initializes database", kTestName);
        const auto makeMessage = [](const char *id, const char *qq) -> insoulforge::json {
            return {{"message_id", id}, {"sender", {{"qq", qq}}},
              {"segments", {{{"type", "text"}, {"text", id}}}}};
        };
        const auto oldMessages = insoulforge::json::array({makeMessage("old", "11")});
        insoulforge::ChatRecordStore::replaceSessionChatRecords(database, 100, oldMessages);
        insoulforge::ChatRecordStore::replaceSessionChatRecords(database, 200, oldMessages);
        const auto oldRecords = insoulforge::ChatRecordStore::getChatRecords(database, 100);
        const auto otherRecords = insoulforge::ChatRecordStore::getChatRecords(database, 200);

        insoulforge::MessageList messages(100, database, insoulforge::Config::instance());
        static_cast<void>(messages.append(makeMessage("new-user", "11")));
        static_cast<void>(messages.append(makeMessage("new-assistant", "self")));
        const auto snapshot = messages.fullSnapshot();
        insoulforge::Statement(database.handle(),
          "CREATE TRIGGER reject_second_chat_record BEFORE INSERT ON chat_records "
          "WHEN NEW.group_id = 100 AND (SELECT COUNT(*) FROM chat_records WHERE group_id = 100) = 1 "
          "BEGIN SELECT RAISE(ABORT, 'test insertion failure'); END").execOrThrow();
        bool failed = false;
        try {
            messages.flushToStorage();
        } catch (const insoulforge::DbError &) {
            failed = true;
        }
        check(failed, "insertion failure propagates", kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(database, 100) == oldRecords,
          "failed replacement restores all old records without partial inserts", kTestName);
        check(messages.fullSnapshot() == snapshot, "failed flush preserves runtime messages", kTestName);
        check(sqlite3_get_autocommit(database.handle()) != 0, "failure leaves no open transaction", kTestName);

        insoulforge::Statement(database.handle(), "DROP TRIGGER reject_second_chat_record").execOrThrow();
        messages.flushToStorage();
        const auto saved = insoulforge::ChatRecordStore::getChatRecords(database, 100);
        check(saved.size() == snapshot.size(), "retry saves complete snapshot", kTestName);
        for (size_t i = 0; i < saved.size(); ++i) {
            check(saved[i]["content"] == insoulforge::dumpJson(snapshot[i]),
              "preserves message content and chronological order", kTestName);
            check(saved[i]["role"] == (i == 2 ? "assistant" : "user"), "preserves message roles", kTestName);
        }
        messages.flushToStorage();
        check(insoulforge::ChatRecordStore::getChatRecords(database, 100) == saved,
          "repeated flush does not duplicate messages", kTestName);
        insoulforge::MessageList restored(100, database, insoulforge::Config::instance());
        check(restored.fullSnapshot() == snapshot, "restores full snapshot after replacement", kTestName);
        insoulforge::ChatRecordStore::replaceSessionChatRecords(database, 100, insoulforge::json::array());
        check(insoulforge::ChatRecordStore::getChatRecords(database, 100).empty(), "empty snapshot clears session",
          kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(database, 200) == otherRecords,
          "replacement and rollback leave other sessions unchanged", kTestName);
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
        check(!messages.createSummaryBatch(), "does not summarize below threshold", kTestName);
        static_cast<void>(messages.append(makeMessage(2)));
        static_cast<void>(messages.append(makeMessage(3)));
        const auto fourth = messages.append(makeMessage(4));
        check(!messages.append(makeMessage(4)), "duplicate message ID returns no snapshot", kTestName);
        const auto batch = messages.createSummaryBatch();

        check(first->size() == 1, "first snapshot stays immutable after later appends", kTestName);
        check(batch.has_value(), "reserves summary when message count exceeds threshold", kTestName);
        check(batch->messages.size() == 2, "summary batch has configured size", kTestName);
        check(batch->messages[0]["message_id"] == "1", "summary batch keeps chronological order", kTestName);
        check(batch->contextMessages.size() == 1, "summary batch includes configured context", kTestName);
        check(!messages.createSummaryBatch(), "does not create another summary while one is pending", kTestName);
        check(fourth->size() == 2, "snapshot retains configured recent window", kTestName);
        check((*fourth)[0]["message_id"] == "3", "snapshot starts at configured context limit", kTestName);
        check((*fourth)[1]["assets"]["images"][0]["source"]["url"] == "https://example.com/image.jpg",
          "complete image source remains available to tools", kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(100).empty(), "runtime append does not write database",
          kTestName);

        messages.finishSummaryBatch(batch->messages.size());
        check(messages.snapshot().size() == 2, "completed summary deletes only its oldest batch", kTestName);
        check(messages.snapshot()[0]["message_id"] == "3", "completed summary retains newer messages", kTestName);

        messages.flushToStorage();
        const auto restoredRecords = insoulforge::ChatRecordStore::getChatRecords(100);
        check(restoredRecords.size() == 2, "flush persists only retained messages", kTestName);
        insoulforge::MessageList restored(100, database, config);
        check(restored.snapshot() == (*fourth), "startup restoration rebuilds retained snapshot", kTestName);

        messages.clear();
        check(messages.fullSnapshot().empty(), "clear removes runtime messages", kTestName);
        check(insoulforge::ChatRecordStore::getChatRecords(100).empty(), "clear removes stored messages", kTestName);
        messages.flushToStorage();
        check(insoulforge::ChatRecordStore::getChatRecords(100).empty(), "flush does not resurrect cleared messages",
          kTestName);

        static_cast<void>(messages.append(makeMessage(5)));
        static_cast<void>(messages.append(makeMessage(6)));
        static_cast<void>(messages.append(makeMessage(7)));
        const auto pending = messages.createSummaryBatch();
        check(pending.has_value(), "reserves summary at exact threshold", kTestName);
        messages.clear();
        static_cast<void>(messages.append(makeMessage(8)));
        check(!messages.createSummaryBatch(), "clear does not release an active task", kTestName);
        messages.finishSummaryBatch(pending->messages.size());
        check(messages.fullSnapshot().empty(), "old completion deletes the current prefix even after manual clear",
          kTestName);

        static_cast<void>(messages.append(makeMessage(9)));
        static_cast<void>(messages.append(makeMessage(10)));
        static_cast<void>(messages.append(makeMessage(11)));
        check(messages.createSummaryBatch().has_value(), "starts a batch at threshold", kTestName);
        messages.finishSummaryBatch(0);
        check(messages.fullSnapshot().size() == 3, "failed task preserves messages", kTestName);
        const auto retried = messages.createSummaryBatch();
        check(retried.has_value(), "a later preparation check can retry a failed batch", kTestName);
        static_cast<void>(messages.append(makeMessage(12)));
        static_cast<void>(messages.append(makeMessage(13)));
        messages.finishSummaryBatch(retried->messages.size());
        check(messages.fullSnapshot().size() == 3, "success deletes only the reserved count", kTestName);
        check(messages.createSummaryBatch().has_value(), "completion does not reserve the next batch", kTestName);
        messages.flushToStorage();
        insoulforge::MessageList restarted(100, database, config);
        check(
          restarted.createSummaryBatch().has_value(), "restart does not restore in-memory execution state", kTestName);

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
        const auto privateId = insoulforge::SessionId::fromPrivateUser(42);
        for (const auto id: {u64{555}, u64{556}, privateId}) {
            insoulforge::SessionStore::enableSession(id);
            insoulforge::SessionStore::setCrossSessionSharing(id, true);
        }
        workflow.appendDeliveredAssistantMessage(556,
          insoulforge::MessageRecord::createAssistantRecord("test", 779, "other group"));
        workflow.appendDeliveredAssistantMessage(privateId,
          insoulforge::MessageRecord::createAssistantRecord("test", 780, "private conversation"));
        const auto shared = insoulforge::SharedConversationContext::build(555, 1);
        const auto sharedGroup = std::ranges::find_if(shared, [](const auto &item) {
            return item["session"]["type"] == "group";
        });
        const auto sharedPrivate = std::ranges::find_if(shared, [](const auto &item) {
            return item["session"]["type"] == "private";
        });
        check(shared.size() == 2 && sharedGroup != shared.end() && sharedPrivate != shared.end() &&
                (*sharedGroup)["session"]["group_id"] == 556 && (*sharedGroup)["messages"].size() == 1 &&
                (*sharedPrivate)["session"]["qq"] == "42",
          "context groups live group and private messages with explicit source identity", kTestName);
        insoulforge::SessionStore::setCrossSessionSharing(555, false);
        check(insoulforge::SharedConversationContext::build(555, 10).empty() && shared.size() == 2,
          "opt-out stops new requests without mutating an existing snapshot", kTestName);
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
    testLogCredentialRedaction();
    testAdminRequestAccess();
    testConfigInitialization();
    testOneBotMessageFormatter();
    testNewWorkflowNormalizesOneBotEvent();
    testNewWorkflowNormalizesOneBotNotices();
    testNewWorkflowDetectsCommands();
    testDefaultPrivateSessionState();
    testSessionReplyCoalescing();
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
    testFileToolStore();
    testLuaToolExecutor();
    testLuaToolExamples();
    testToolRegistryReload();
    testFinalRoundToolPolicy();
    testIterationRequestMessages();
    testToolRegistryReturnsHandlerErrors();
    testCharacterImageStore();
    testHttpTraceImageRedaction();
    testCharacterImageToolRequiresUpload();
    testQzoneToolValidation();
    testBuiltinToolConfig();
    testMemoryMaintenanceStore();
    testGlobalPersonMemory();
    testPersonConsolidationStore();
    testSessionShortTermMemory();
    testRemovedShortTermLimit();
    testMemoryModelConfigMigration();
    testAffinitySwitch();
    testImageApiConfig();
    testConfigFacade();
    testExecutionConfig();
    testGroupCardSettings();
    testGroupAutoReply();
    testGroupCommandSettings();
    testCrossSessionSharing();
    testGroupAdditionalPrompt();
    testToolHistoryRecords();
    testImageDescriptionCacheStore();
    testImageGenerationResponse();
    testUsageSummaryUsesLatestRoleModel();
    testMessageListSnapshotsAndPersistence();
    testAgentMessageSnapshotOwnership();
    testMessageSnapshotAtomicReplacement();
    testAsyncTaskSessionExclusivity();
    testStartupResults();
    if (failures == 0) {
        std::cout << "All message contract tests passed\n";
        return 0;
    }
    std::cerr << failures << " contract test(s) failed\n";
    return 1;
}
