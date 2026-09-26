/// @file MessageRouter.cpp
/// @brief 基于完整消息快照的 Router 实现

#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <llm/LlmClient.hpp>
#include <llm/prompts/PromptService.hpp>

namespace insoulforge::MessageRouter {
    namespace {
        [[nodiscard]] auto makeDecision(const RouterDecision::Action action, std::string reason,
          const i32 maxLength = 25, const bool priority = false) -> RouterDecision {
            return {.action = action,
              .reason = std::move(reason),
              .shouldReply = action == RouterDecision::Action::REPLY,
              .maxLength = maxLength,
              .isPriority = priority};
        }

        [[nodiscard]] auto applySessionType(RouterDecision decision, const u64 sessionId) -> RouterDecision {
            decision.isPrivate = SessionId::isPrivate(sessionId);
            return decision;
        }

        /// @brief 统计 UTF-8 字符数量，不将多字节汉字误计为多个字符
        [[nodiscard]] auto utf8CharacterCount(const std::string_view text) -> size_t {
            return static_cast<size_t>(std::ranges::count_if(
              text, [](const unsigned char character) -> bool { return (character & 0xC0U) != 0x80U; }));
        }

        [[nodiscard]] auto isSpam(const json &message) -> bool {
            const bool hasNonTextSegment = std::ranges::any_of(atOrNull(message, "segments"),
              [](const json &segment) -> bool { return getStr(segment, "type") != "text"; });
            if (hasNonTextSegment) {
                return false;
            }
            std::string text = MessageRecord::extractText(message);
            std::erase_if(
              text, [](const char character) -> int { return std::isspace(static_cast<unsigned char>(character)); });
            return text.empty() || utf8CharacterCount(text) <= 2;
        }

        [[nodiscard]] auto compactMessage(const json &message) -> json {
            const json projected = MessageRecord::projectForAgent(message);
            json compact;
            compact["message_id"] = getStr(message, "message_id");
            if (const std::string name = getStr(atOrNull(projected, "sender"), "name"); !name.empty()) {
                compact["sender"] = name;
            }
            json content;
            if (const json &segments = atOrNull(projected, "segments"); segments.is_array() && !segments.empty()) {
                content["segments"] = segments;
            }
            compact["content"] = std::move(content);
            return compact;
        }

        [[nodiscard]] auto buildPrompt(
          const u64 sessionId, const std::string_view triggerMessageId, const json &snapshot) -> json {
            const auto &config = Config::instance();
            const size_t keep = static_cast<size_t>(config.routerWindowKeepCount);
            const size_t slide = std::max<size_t>(1, static_cast<size_t>(config.routerWindowTriggerCount) - keep);
            const size_t windowSize = keep + snapshot.size() % slide;
            const size_t startIndex = snapshot.size() > windowSize ? snapshot.size() - windowSize : 0;

            json records = json::array();
            bool spokeInWindow = false;
            size_t silentCount = 0;
            for (const json &message: snapshot | std::views::drop(startIndex)) {
                if (MessageRecord::isAssistant(message)) {
                    spokeInWindow = true;
                    silentCount = 0;
                } else {
                    ++silentCount;
                }
                json record = compactMessage(message);
                if (getStr(message, "message_id") == triggerMessageId) {
                    record["is_current"] = true;
                }
                records.push_back(std::move(record));
            }

            json context;
            context["trigger_message_id"] = triggerMessageId;
            context["chat_records"] = std::move(records);
            context["bot_silence"] = {{"spoke_in_window", spokeInWindow},
              {"messages_since_last_speak", spokeInWindow ? silentCount : snapshot.size() - startIndex}};

            json prompt = json::array();
            prompt.push_back({{"role", "system"},
              {"content", SessionId::isPrivate(sessionId) ? PromptService::getRouterPrivateSystemPrompt()
                                                          : PromptService::getRouterSystemPrompt()}});
            prompt.push_back({{"role", "system"},
              {"content",
                "本轮只判断 trigger_message_id 指定的触发消息；chat_records 中 is_current 为 true 的记录就是该消息。"
                "其后的记录仅用于理解上下文，不应改变本轮判断对象。"}});
            prompt.push_back({{"role", "user"}, {"content", dumpJson(context)}});
            return prompt;
        }

        [[nodiscard]] auto parseDecision(const json &response) -> std::optional<RouterDecision> {
            const std::string content = jsonToString(atOrNull(atOrNull(response["choices"][0], "message"), "content"));
            std::string payload;
            if (!tryExtractJsonObject(content, payload)) {
                return std::nullopt;
            }
            json parsed;
            if (!tryParseJson(payload, parsed)) {
                return std::nullopt;
            }

            std::string action = getStr(parsed, "action", "reply");
            std::ranges::transform(action, action.begin(),
              [](const unsigned char character) -> char { return static_cast<char>(std::tolower(character)); });
            const RouterDecision::Action decisionAction =
              action == "skip" ? RouterDecision::Action::SKIP : RouterDecision::Action::REPLY;
            RouterDecision decision = makeDecision(decisionAction, getStr(parsed, "reason"));
            if (const json &strategy = atOrNull(parsed, "strategy"); strategy.is_object()) {
                decision.tone = getStr(strategy, "tone", "friendly");
                decision.maxLength = std::clamp(getInt(strategy, "maxLength", 25), 10, 500);
            }
            return decision;
        }
    } // namespace

    auto route(const u64 sessionId, const std::string_view triggerMessageId, const json &snapshot)
      -> drogon::Task<RouterDecision> {
        if (!snapshot.is_array() || snapshot.empty()) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "消息快照为空"), sessionId);
        }

        const auto trigger = std::ranges::find_if(snapshot, [triggerMessageId](const json &message) -> bool {
            return getStr(message, "message_id") == triggerMessageId;
        });
        if (trigger == snapshot.end()) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "找不到触发消息"), sessionId);
        }
        if (MessageRecord::isSystem(*trigger)) {
            co_return applySessionType(
              makeDecision(RouterDecision::Action::REPLY, "系统定时任务触发", 100, true), sessionId);
        }
        if (MessageRecord::mentions(*trigger, Config::instance().selfQQNumber)) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "用户@提及", 100, true), sessionId);
        }
        if (!SessionId::isPrivate(sessionId) && isSpam(*trigger)) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "刷屏或短文本"), sessionId);
        }

        const auto &config = Config::instance();
        const auto response = std::optional{co_await LlmClient::requestChat("Router", "router", config.router,
          config.routerParams, buildPrompt(sessionId, triggerMessageId, snapshot), {}, sessionId)};
        if (!response) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "Router 请求失败"), sessionId);
        }
        co_return applySessionType(
          parseDecision(*response).value_or(makeDecision(RouterDecision::Action::REPLY, "Router 响应无效")), sessionId);
    }
} // namespace insoulforge::MessageRouter
