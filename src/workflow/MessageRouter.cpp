/// @file MessageRouter.cpp
/// @brief 基于完整消息快照的 Router 实现

#include <algorithm>
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
    [[nodiscard]] auto windowStartIndex(const size_t snapshotSize) -> size_t {
        const auto &config = Config::instance();
        const size_t keep = static_cast<size_t>(config.routerWindowKeepCount);
        const size_t slide = std::max<size_t>(1, static_cast<size_t>(config.routerWindowTriggerCount) - keep);
        const size_t windowSize = keep + snapshotSize % slide;
        return snapshotSize > windowSize ? snapshotSize - windowSize : 0;
    }

    namespace {
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

        [[nodiscard]] auto compactMessage(const json &message, const bool includeGroupCard) -> json {
            const json projected = MessageRecord::projectForAgent(message, false, includeGroupCard);
            json compact;
            if (const auto card = getStr(atOrNull(projected, "sender"), "card"); !card.empty()) {
                compact["card"] = card;
            }
            if (const auto role = getStr(atOrNull(projected, "sender"), "role"); !role.empty()) {
                compact["role"] = role;
            }
            if (message.contains("time")) {
                compact["time"] = message["time"];
            }
            if (projected.contains("reply_to")) {
                compact["reply_to"] = projected["reply_to"];
            }
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

        /// @brief 将 Agent 可见的消息段转为 Jev 可读的文本。
        [[nodiscard]] auto renderJevSegment(const json &segment) -> std::string {
            const std::string type = getStr(segment, "type");
            if (type == "text") {
                return getStr(segment, "text");
            }
            if (type == "face") {
                const std::string label = getStr(segment, "label", getStr(segment, "id"));
                return fmt::format("[表情:{}]", label);
            }
            if (type == "sticker") {
                return fmt::format("[贴纸:{}]", getStr(segment, "name"));
            }
            if (type == "image") {
                if (getStr(segment, "recognition_status") == "succeeded") {
                    return fmt::format("[图片:{}]", getStr(segment, "description"));
                }
                return "[图片]";
            }
            if (type == "at") {
                const json &target = atOrNull(segment, "target");
                if (getStr(target, "kind") == "all") {
                    return "@全体";
                }
                const std::string name = getStr(target, "name");
                return fmt::format("@{}", name.empty() ? getStr(target, "user_id") : name);
            }
            if (type == "poke") {
                const json &target = atOrNull(segment, "target");
                const std::string name = getStr(target, "name");
                return fmt::format("[戳一戳:{}]", name.empty() ? getStr(target, "user_id") : name);
            }
            if (type == "member_event") {
                return fmt::format("[群事件:{}]", getStr(segment, "action"));
            }
            if (type == "unsupported") {
                return fmt::format("[不支持:{}]", getStr(segment, "segment_type"));
            }
            return "";
        }

        /// @brief 压平消息段，保留消息 ID 与引用 ID 以便识别回复关系。
        [[nodiscard]] auto renderJevRecord(const json &message, const bool includeGroupCard) -> json {
            const json projected = MessageRecord::projectForAgent(message, false, includeGroupCard);
            std::string text;
            for (const json &segment: atOrNull(projected, "segments")) {
                text += renderJevSegment(segment);
            }

            json record;
            if (const json &messageId = atOrNull(projected, "message_id"); !messageId.is_null()) {
                record["message_id"] = messageId;
            }
            if (MessageRecord::isAssistant(message)) {
                record["sender"] = fmt::format("[bot]{}", Config::instance().botName);
            } else if (const std::string name = getStr(atOrNull(projected, "sender"), "name"); !name.empty()) {
                record["sender"] = name;
            } else {
                record["sender"] = getStr(atOrNull(projected, "sender"), "user_id");
            }
            if (const auto card = getStr(atOrNull(projected, "sender"), "card"); !card.empty()) {
                record["card"] = card;
            }
            if (const auto role = getStr(atOrNull(projected, "sender"), "role"); !role.empty()) {
                record["role"] = role;
            }
            if (projected.contains("time")) {
                record["time"] = projected["time"];
            }
            record["text"] = std::move(text);
            if (const json &replyTo = atOrNull(projected, "reply_to"); !replyTo.is_null() && !replyTo.empty()) {
                record["reply_to"] = replyTo;
            }
            return record;
        }

        /// @brief 使用独立的最近 N 条滑动窗口构建 Jev 状态。
        [[nodiscard]] auto buildJevState(const SessionId &sessionId, const json &snapshot) -> json {
            const bool includeGroupCard =
              !sessionId.isPrivate() && SessionStore::getSessionConfig(sessionId).sendGroupCard;
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
                auto &target = index >= sentStart                         ? sentMessages
                               : MessageRecord::isNewForExecutor(message) ? newMessages
                                                                          : records;
                target.push_back(renderJevRecord(message, includeGroupCard));
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
            action["instructions"] =
              "以下是群聊上下文。chat_records 是历史，new_messages 是尚未交给回复生成器的入站消息，sent_messages "
              "是末尾连续已发送的回复。"
              "reply_to 是被引用消息的 message_id，[bot] 标记机器人发言。"
              "结合已经发送的内容，判断当前是否仍有必要回复；各组时间可能交错，不按字段顺序推断消息先后。";
            const std::string &botName = Config::instance().botName;
            action["criteria"] = {
              {"skip", fmt::format("纯表情/贴纸、单字或复读、广告、群友互聊且未涉及{0}、"
                                   "{0}刚发言且新消息非针对它、无意义闲聊",
                         botName)},
              {"reply", fmt::format("被@或叫到名字、有提问、分享{0}可能感兴趣的内容、对{0}上次发言的反馈、"
                                    "有梗可发挥、{0}久未发言或冷场需活跃",
                          botName)},
              {"unclear", "以上都不明显适用，或聊天记录不足以判断"},
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

        [[nodiscard]] auto buildPrompt(const SessionId &sessionId, const json &snapshot) -> json {
            const bool includeGroupCard =
              !sessionId.isPrivate() && SessionStore::getSessionConfig(sessionId).sendGroupCard;
            const size_t startIndex = windowStartIndex(snapshot.size());

            json records = json::array();
            json newMessages = json::array();
            json sentMessages = json::array();
            const auto sentStart = MessageRecord::sentMessagesStart(snapshot);
            for (size_t index = startIndex; index < snapshot.size(); ++index) {
                const auto &message = snapshot[index];
                auto &target = index >= sentStart                         ? sentMessages
                               : MessageRecord::isNewForExecutor(message) ? newMessages
                                                                          : records;
                target.push_back(compactMessage(message, includeGroupCard));
            }

            const auto [spokeInWindow, silentCount] = computeBotSilence(snapshot, startIndex);

            json context;
            context["chat_records"] = std::move(records);
            context["new_messages"] = std::move(newMessages);
            context["sent_messages"] = std::move(sentMessages);
            context["bot_silence"] = {{"spoke_in_window", spokeInWindow},
              {"messages_since_last_speak", spokeInWindow ? silentCount : snapshot.size() - startIndex}};

            json prompt = json::array();
            prompt.push_back(
              {{"role", "system"}, {"content", sessionId.isPrivate() ? PromptService::getRouterPrivateSystemPrompt()
                                                                     : PromptService::getRouterSystemPrompt()}});
            prompt.push_back({{"role", "system"},
              {"content", "chat_records 是历史；new_messages 是尚未交给回复生成器的入站消息；sent_messages "
                          "是末尾连续成功发送的回复。"
                          "结合全部上下文和已发送内容判断当前是否还需回复，不指定回复某条消息。各组时间可能交错，不按字"
                          "段顺序推断先后。"}});
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

        // 群聊先由 Jev 判断；无法确定时沿用原有 Router LLM。
        if (!sessionId.isPrivate() && JevClient::isConfigured(config.jev)) {
            const auto jevResponse = co_await JevClient::requestSystemOne(
              config.jev, buildJevState(sessionId, snapshot), buildJevQuestions(), sessionId);
            if (jevResponse) {
                const auto choice = JevClient::readChoice(
                  *jevResponse, std::string(kJevActionQuestion), {kJevActionSkip, kJevActionReply, kJevActionUnclear});
                const auto confidence = JevClient::readConfidence(*jevResponse, std::string(kJevActionQuestion));
                const auto action =
                  choice ? classifyJevChoice(*choice, confidence, config.jevMinConfidence) : std::nullopt;
                Logger::info(sessionId, "Jev",
                  fmt::format("choice={} | confidence={} | threshold={:.2f} | model={} | fallback={}",
                    choice.value_or("n/a"), confidence ? fmt::format("{:.4f}", *confidence) : "n/a",
                    config.jevMinConfidence, getStr(*jevResponse, "model", config.jev.model), action ? "no" : "yes"));
                if (action) {
                    co_return applySessionType(makeJevDecision(*jevResponse, *action), sessionId);
                }
            } else {
                Logger::info(sessionId, "Jev",
                  fmt::format("choice=n/a | confidence=n/a | threshold={:.2f} | model={} | fallback=yes",
                    config.jevMinConfidence, config.jev.model));
            }
        }

        // 保持原有 fail-open 行为：LLM 请求失败或响应无效时仍尝试回复。
        const auto response = std::optional{co_await LlmClient::requestChat(
          "Router", "router", config.router, config.routerParams, buildPrompt(sessionId, snapshot), {}, sessionId)};
        if (!response) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "Router 请求失败"), sessionId);
        }
        co_return applySessionType(
          parseDecision(*response).value_or(makeDecision(RouterDecision::Action::REPLY, "Router 响应无效")), sessionId);
    }
} // namespace insoulforge::MessageRouter
