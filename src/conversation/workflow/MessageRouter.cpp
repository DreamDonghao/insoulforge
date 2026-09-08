/// @file MessageRouter.cpp
/// @brief 基于完整消息快照的 Router 实现

#include <algorithm>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <infrastructure/config/Config.hpp>
#include <llm/LlmClient.hpp>
#include <llm/PromptService.hpp>

namespace insoulforge::MessageRouter {
    namespace {
        [[nodiscard]] RouterDecision makeDecision(const RouterDecision::Action action, std::string reason,
          const int maxLength = 25, const bool priority = false) {
            return {.action = action,
              .reason = std::move(reason),
              .shouldReply = action == RouterDecision::Action::REPLY,
              .maxLength = maxLength,
              .isPriority = priority};
        }

        [[nodiscard]] RouterDecision applySessionType(RouterDecision decision, const uint64_t sessionId) {
            decision.isPrivate = SessionId::isPrivate(sessionId);
            return decision;
        }

        /// @brief 统计 UTF-8 字符数量，不将多字节汉字误计为多个字符
        [[nodiscard]] size_t utf8CharacterCount(const std::string_view text) {
            return static_cast<size_t>(
              std::ranges::count_if(text, [](const unsigned char character) { return (character & 0xC0U) != 0x80U; }));
        }

        [[nodiscard]] bool isSpam(const json &message) {
            const bool hasNonTextSegment = std::ranges::any_of(
              atOrNull(message, "segments"), [](const json &segment) { return getStr(segment, "type") != "text"; });
            if (hasNonTextSegment) {
                return false;
            }
            std::string text = MessageRecord::extractText(message);
            std::erase_if(
              text, [](const char character) { return std::isspace(static_cast<unsigned char>(character)); });
            return text.empty() || utf8CharacterCount(text) <= 2;
        }

        [[nodiscard]] json compactMessage(const json &message) {
            const json projected = MessageRecord::projectForAgent(message);
            json compact;
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

        [[nodiscard]] json buildPrompt(const uint64_t sessionId, const json &snapshot) {
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
                records.push_back(compactMessage(message));
            }
            if (!records.empty()) {
                records.back()["is_current"] = true;
            }

            json context;
            context["chat_records"] = std::move(records);
            context["bot_silence"] = {{"spoke_in_window", spokeInWindow},
              {"messages_since_last_speak", spokeInWindow ? silentCount : snapshot.size() - startIndex}};

            json prompt = json::array();
            prompt.push_back({{"role", "system"},
              {"content", SessionId::isPrivate(sessionId) ? PromptService::getRouterPrivateSystemPrompt()
                                                          : PromptService::getRouterSystemPrompt()}});
            prompt.push_back({{"role", "user"}, {"content", dumpJson(context)}});
            return prompt;
        }

        [[nodiscard]] std::optional<RouterDecision> parseDecision(const json &response) {
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
              [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
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

    drogon::Task<RouterDecision> route(const uint64_t sessionId, const json &snapshot) {
        if (!snapshot.is_array() || snapshot.empty()) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "消息快照为空"), sessionId);
        }

        const json &message = snapshot.back();
        if (MessageRecord::isSystem(message)) {
            co_return applySessionType(
              makeDecision(RouterDecision::Action::REPLY, "系统定时任务触发", 100, true), sessionId);
        }
        if (MessageRecord::isAssistant(message)) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "机器人自身消息"), sessionId);
        }
        if (MessageRecord::mentions(message, Config::instance().selfQQNumber)) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "用户@提及", 100, true), sessionId);
        }
        if (MessageRecord::hasSegmentType(message, "face")) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "用户发送原生表情"), sessionId);
        }
        if (!SessionId::isPrivate(sessionId) && isSpam(message)) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "刷屏或短文本"), sessionId);
        }

        const auto &config = Config::instance();
        const auto response = co_await LlmClient::requestChat(
          "Router", "router", config.router, config.routerParams, buildPrompt(sessionId, snapshot), {}, sessionId);
        if (!response) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "Router 请求失败"), sessionId);
        }
        co_return applySessionType(
          parseDecision(*response).value_or(makeDecision(RouterDecision::Action::REPLY, "Router 响应无效")), sessionId);
    }
} // namespace insoulforge::MessageRouter
