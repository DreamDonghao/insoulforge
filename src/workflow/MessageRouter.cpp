/// @file MessageRouter.cpp
/// @brief 基于完整消息快照的 Router 实现

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/UserId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/MessageRouter.hpp>
#include <workflow/agent/llm/JevClient.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::MessageRouter {
    namespace {
        constexpr std::string_view kGroupRoutingTask = "判断群聊中的机器人接下来是否需要回复。\n\n";
        constexpr std::string_view kContextInstructions =
          "sender 以 [bot] 开头表示机器人。\n"
          "new_messages 是机器人收到、还没纳入任何一轮回复生成的消息。\n"
          "sent_messages 是机器人在最近一条收到的消息之后发出的消息。\n"
          "chat_records 是其余聊天记录。\n"
          "reply_to 指向被引用消息的 message_id。\n"
          "bot_silence.messages_since_last_speak 是这批记录中，最近一条机器人记录之后的消息条数。"
          "如果这批记录里没有机器人记录，则取消息总数。";

        constexpr std::string_view kJevActionSkip = "skip";
        constexpr std::string_view kJevActionReply = "reply";
        constexpr std::string_view kJevActionUnclear = "unclear";

        [[nodiscard]] auto makeDecision(const RouterDecision::Action action, std::string reason,
          const std::optional<i32> maxLength = 25, const bool priority = false) -> RouterDecision {
            return {.action = action,
              .reason = std::move(reason),
              .shouldReply = action == RouterDecision::Action::REPLY,
              .maxLength = maxLength,
              .isPriority = priority};
        }

        [[nodiscard]] auto applySessionType(RouterDecision decision, const SessionId &sessionId) -> RouterDecision {
            decision.isPrivate = sessionId.isPrivate();
            return decision;
        }

        /// @brief 统计窗口内机器人是否发言，以及此后的消息数。
        [[nodiscard]] auto computeBotSilence(const json &snapshot, const size_t startIndex) -> std::pair<bool, size_t> {
            bool spokeInWindow = false;
            size_t silentCount = 0;
            for (const json &message: snapshot | std::views::drop(startIndex)) {
                if (MessageRecord::isAssistant(message)) {
                    spokeInWindow = true;
                    silentCount = 0;
                } else {
                    ++silentCount;
                }
            }
            return {spokeInWindow, silentCount};
        }

        constexpr std::string_view kJevActionQuestion = "action";
        constexpr std::string_view kJevToneQuestion = "tone";
        constexpr std::string_view kJevMaxLengthQuestion = "maxLength";

        /// @brief 将消息段压平为路由模型可读的文本。
        [[nodiscard]] auto renderRouterSegment(const json &segment) -> std::string {
            const std::string type = getStr(segment, "type");
            if (type == "text") {
                return getStr(segment, "text");
            }
            if (type == "face") {
                const std::string label = getStr(segment, "label", getStr(segment, "id"));
                return std::format("[表情:{}]", label);
            }
            if (type == "sticker") {
                return std::format("[贴纸:{}]", getStr(segment, "name"));
            }
            if (type == "audio") {
                const auto transcript = getStr(segment, "transcript");
                return transcript.empty() ? "[语音]" : std::format("[语音:{}]", transcript);
            }
            if (type == "image") {
                if (getStr(segment, "recognition_status") == "succeeded") {
                    return std::format("[图片:{}]", getStr(segment, "description"));
                }
                return "[图片]";
            }
            if (type == "at") {
                const json &target = atOrNull(segment, "target");
                if (getStr(target, "kind") == "all") {
                    return "@全体";
                }
                const std::string name = getStr(target, "name");
                return std::format("@{}", name.empty() ? getStr(target, "user_id") : name);
            }
            if (type == "poke") {
                const json &target = atOrNull(segment, "target");
                const std::string name = getStr(target, "name");
                return std::format("[戳一戳:{}]", name.empty() ? getStr(target, "user_id") : name);
            }
            if (type == "member_event") {
                return std::format("[群事件:{}]", getStr(segment, "action"));
            }
            if (type == "unsupported") {
                return std::format("[不支持:{}]", getStr(segment, "segment_type"));
            }
            return "";
        }

        /// @brief 压平消息段，保留消息 ID 与引用 ID 以便识别回复关系。
        [[nodiscard]] auto renderRouterRecord(const json &message) -> json {
            const json projected = MessageRecord::projectForAgent(message);
            std::string text;
            for (const json &segment: atOrNull(projected, "segments")) {
                text += renderRouterSegment(segment);
            }

            json record;
            if (const json &messageId = atOrNull(projected, "message_id"); !messageId.is_null()) {
                record["message_id"] = messageId;
            }
            if (MessageRecord::isAssistant(message)) {
                record["sender"] = std::format("[bot]{}", Config::instance().botName);
            } else if (const std::string name = getStr(atOrNull(projected, "sender"), "name"); !name.empty()) {
                record["sender"] = name;
            } else {
                record["sender"] = getStr(atOrNull(projected, "sender"), "user_id");
            }
            if (projected.contains("activity")) {
                record["activity"] = {{"mode", getStr(projected["activity"], "mode")}};
            }
            if (projected.contains("send_error")) {
                record["send_error"] = projected["send_error"];
            }
            record["text"] = std::move(text);
            if (const json &replyTo = atOrNull(projected, "reply_to"); !replyTo.is_null() && !replyTo.empty()) {
                record["reply_to"] = replyTo;
            }
            return record;
        }

        /// @brief 构建 Jev 与 Router LLM 共用的最近 N 条消息上下文。
        [[nodiscard]] auto buildRouterContext(const json &snapshot) -> json {
            const auto windowSize =
              static_cast<size_t>(std::max(1, Config::instance().jevWindowSize.load(std::memory_order_relaxed)));
            const size_t startIndex = snapshot.size() > windowSize ? snapshot.size() - windowSize : 0;
            const auto [spokeInWindow, silentCount] = computeBotSilence(snapshot, startIndex);

            json records = json::array();
            json newMessages = json::array();
            json sentMessages = json::array();
            const auto sentStart = MessageRecord::sentMessagesStart(snapshot);
            for (size_t index = startIndex; index < snapshot.size(); ++index) {
                const auto &message = snapshot[index];
                auto &target = index >= sentStart && MessageRecord::isConfirmedSend(message) ? sentMessages
                               : MessageRecord::isNewForExecutor(message)                    ? newMessages
                                                                                             : records;
                target.push_back(renderRouterRecord(message));
            }

            json state;
            state["chat_records"] = std::move(records);
            state["new_messages"] = std::move(newMessages);
            state["sent_messages"] = std::move(sentMessages);
            state["bot_silence"] = {{"spoke_in_window", spokeInWindow},
              {"messages_since_last_speak", spokeInWindow ? silentCount : snapshot.size() - startIndex}};
            return state;
        }

        /// @brief 一次请求判断是否回复，并给回复生成器提供语气与长度建议。
        [[nodiscard]] auto buildJevQuestions() -> json {
            json action;
            action["type"] = "choice";
            action["instructions"] = std::string(kGroupRoutingTask).append(kContextInstructions);
            const std::string &botName = Config::instance().botName;
            action["criteria"] = {
              {"skip", "值得回应的内容已全部被回复覆盖，或仅剩广告、刷屏、无实质内容的表情复读及不宜插入的群友互聊"},
              {"reply",
                std::format("仍有未覆盖且值得{0}回应的问题、补充或反馈，或有感兴趣的分享、有梗可发挥的话题值得自然接续",
                  botName)},
              {"unclear", "无法确定已发送内容是否覆盖问题，或是否仍有值得回应的内容"},
            };

            json tone;
            tone["type"] = "choice";
            tone["instructions"] = "如果需要回复，应该使用什么语气？";
            tone["criteria"] = {
              {"friendly", "亲切自然"}, {"serious", "认真回答问题或求助"}, {"casual", "轻松随意地接话"}};

            json maxLength;
            maxLength["type"] = "choice";
            maxLength["instructions"] = "如果需要回复，回复的字数上限设为多少合适？";
            maxLength["criteria"] = {{"15", "简短接话或打招呼"}, {"25", "普通闲聊"}, {"50", "简单问题或一般聊天"},
              {"100", "认真回答或讨论"}, {"200", "需要详细说明的复杂问题"}, {"500", "创作、写作或翻译等长文本"}};

            return json{{std::string(kJevActionQuestion), std::move(action)},
              {std::string(kJevToneQuestion), std::move(tone)},
              {std::string(kJevMaxLengthQuestion), std::move(maxLength)}};
        }

        [[nodiscard]] auto buildPrompt(const SessionId &sessionId, const json &context) -> json {
            auto systemPrompt = sessionId.isPrivate() ? PromptService::getRouterPrivateSystemPrompt()
                                                      : PromptService::getRouterSystemPrompt();
            systemPrompt.append("\n\n").append(kContextInstructions);
            return json::array({{{"role", "system"}, {"content", std::move(systemPrompt)}},
              {{"role", "user"}, {"content", dumpJson(context)}}});
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

        /// @brief 将 Jev 的回复建议转为 RouterDecision；跳过时无需读取建议。
        [[nodiscard]] auto makeJevDecision(const json &jevResponse, const RouterDecision::Action action)
          -> RouterDecision {
            RouterDecision decision = makeDecision(action, "Jev 优先判定");
            if (action != RouterDecision::Action::REPLY) {
                return decision;
            }
            if (const auto tone = JevClient::readChoice(
                  jevResponse, std::string(kJevToneQuestion), {"friendly", "serious", "casual"})) {
                decision.tone = *tone;
            }
            if (const auto maxLengthLabel = JevClient::readChoice(
                  jevResponse, std::string(kJevMaxLengthQuestion), {"15", "25", "50", "100", "200", "500"})) {
                decision.maxLength = std::clamp(jsonToInt(json(*maxLengthLabel), 25), 10, 500);
            }
            return decision;
        }
    } // namespace

    auto classifyJevChoice(const std::string_view label, const std::optional<double> confidence,
      const f64 minConfidence) -> std::optional<RouterDecision::Action> {
        if (!confidence || !std::isfinite(*confidence) || *confidence < minConfidence) {
            return std::nullopt;
        }
        if (label == kJevActionReply) {
            return RouterDecision::Action::REPLY;
        }
        if (label == kJevActionSkip) {
            return RouterDecision::Action::SKIP;
        }
        return std::nullopt;
    }

    auto route(const SessionId sessionId, const json &snapshot) -> drogon::Task<RouterDecision> {
        if (!snapshot.is_array() || snapshot.empty()) {
            co_return applySessionType(makeDecision(RouterDecision::Action::SKIP, "消息快照为空"), sessionId);
        }

        bool hasRoutableInput = false;
        // 只对未交给 Executor 的入站消息应用硬规则，已处理的历史提及不能再次强制触发。
        for (const auto &message: snapshot | std::views::reverse) {
            if (!MessageRecord::isNewForExecutor(message)) {
                continue;
            }
            const auto mode = getStr(atOrNull(message, "activity"), "mode");
            if (mode == "silent") {
                continue;
            }
            hasRoutableInput = true;
            if (mode == "judge") {
                continue;
            }
            if (mode == "force") {
                co_return applySessionType(
                  makeDecision(RouterDecision::Action::REPLY, "Activity 通知", std::nullopt, true), sessionId);
            }
            if (MessageRecord::isSystem(message)) {
                co_return applySessionType(
                  makeDecision(RouterDecision::Action::REPLY, "系统消息触发", 100, true), sessionId);
            }
            if (sessionId.isPrivate()) {
                co_return applySessionType(
                  makeDecision(RouterDecision::Action::REPLY, "私聊直接执行", std::nullopt, true), sessionId);
            }
            if (MessageRecord::mentions(message, UserId{"self"})) {
                co_return applySessionType(
                  makeDecision(RouterDecision::Action::REPLY, "用户@提及", std::nullopt, true), sessionId);
            }
        }

        if (!hasRoutableInput) {
            co_return applySessionType(
              makeDecision(RouterDecision::Action::SKIP, "无需要判断的未处理入站消息"), sessionId);
        }

        // 必须回复的硬规则优先于群自动回复开关、Jev 和 Router LLM。
        if (!sessionId.isPrivate() && !SessionStore::getSessionConfig(sessionId).autoReply) {
            co_return makeDecision(RouterDecision::Action::SKIP, "本群已关闭自动回复");
        }
        const auto &config = Config::instance();
        const auto context = buildRouterContext(snapshot);

        // 群聊先由 Jev 判断；无法确定时沿用原有 Router LLM。
        if (const auto jev = sessionId.isPrivate() ? std::nullopt : config.enabledJevRoutingConfig();
          jev && JevClient::isConfigured(jev->api)) {
            const auto jevResponse =
              co_await JevClient::requestSystemOne(jev->api, context, buildJevQuestions(), sessionId);
            if (jevResponse) {
                const auto choice = JevClient::readChoice(
                  *jevResponse, std::string(kJevActionQuestion), {kJevActionSkip, kJevActionReply, kJevActionUnclear});
                const auto confidence = JevClient::readConfidence(*jevResponse, std::string(kJevActionQuestion));
                const auto action = choice ? classifyJevChoice(*choice, confidence, jev->minConfidence) : std::nullopt;
                Logger::info(sessionId, "Jev",
                  std::format("choice={} | confidence={} | threshold={:.2f} | model={} | fallback={}",
                    choice.value_or("n/a"), confidence ? std::format("{:.4f}", *confidence) : "n/a", jev->minConfidence,
                    getStr(*jevResponse, "model", jev->api.model), action ? "no" : "yes"));
                if (action) {
                    co_return applySessionType(makeJevDecision(*jevResponse, *action), sessionId);
                }
            } else {
                Logger::info(sessionId, "Jev",
                  std::format("choice=n/a | confidence=n/a | threshold={:.2f} | model={} | fallback=yes",
                    jev->minConfidence, jev->api.model));
            }
        }

        // 保持原有 fail-open 行为：LLM 请求失败或响应无效时仍尝试回复。
        const auto response = co_await LlmClient::requestChat(
          "Router", "router", config.router, config.routerParams, buildPrompt(sessionId, context), {}, sessionId);
        if (!response) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "Router 请求失败"), sessionId);
        }
        co_return applySessionType(
          parseDecision(*response).value_or(makeDecision(RouterDecision::Action::REPLY, "Router 响应无效")), sessionId);
    }
} // namespace insoulforge::MessageRouter
