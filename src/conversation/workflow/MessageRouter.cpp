/// @file MessageRouter.cpp
/// @brief 基于完整消息快照的 Router 实现

#include <algorithm>
#include <ranges>
#include <utility>

#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/JevClient.hpp>
#include <llm/LlmClient.hpp>
#include <llm/prompts/PromptService.hpp>

namespace insoulforge::MessageRouter {
    /// @brief 计算 Router 上下文窗口的起始下标
    /// @param snapshotSize 完整快照的消息条数
    /// @details 该窗口按消息条数分段扩展：窗口大小在 [keep, keep + slide - 1] 之间周期性
    ///          变化（默认 10~19），同一批次内起始下标保持不变，只有跨越批次边界时前缀
    ///          才整体前移。此设计意在保留稳定的上下文前缀，提高请求缓存命中率；逐条
    ///          滑动会让聊天记录部分几乎每轮都变，命中率显著下降。
    ///          优先路径（Jev）与兜底路径（LLM）共用这一语义，均通过本函数取窗口起点。
    ///          导出到头文件用于支持无网络的离线测试，保持 route() 的既有签名不变。
    [[nodiscard]] auto windowStartIndex(const size_t snapshotSize) -> size_t {
        const auto &config = Config::instance();
        const size_t keep = static_cast<size_t>(config.routerWindowKeepCount);
        const size_t slide = std::max<size_t>(1, static_cast<size_t>(config.routerWindowTriggerCount) - keep);
        const size_t windowSize = keep + snapshotSize % slide;
        return snapshotSize > windowSize ? snapshotSize - windowSize : 0;
    }

    namespace {
        /// @brief Jev action choice 的 label 常量，集中定义一处以避免字符串双写
        /// @details 供构建 criteria 与读取胜出 label 两处共用；kJevActionUnclear 本票
        ///          暂未使用，留作接入时（接入 route() 分支、构建 criteria）共用。
        constexpr std::string_view kJevActionSkip = "skip";
        constexpr std::string_view kJevActionReply = "reply";
        [[maybe_unused]] constexpr std::string_view kJevActionUnclear = "unclear";

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

        /// @brief 计算机器人在上下文窗口内的静默状态（离散预计算值，不传原始计数）
        /// @param snapshot 完整消息快照
        /// @param startIndex 上下文窗口起始下标（windowStartIndex 的产出）
        /// @return {窗口内是否发过言, 自上次发言后的沉默条数}
        /// @details 与 buildPrompt() 共用，buildPrompt() 的产出逐字节不变。
        ///          Jev 侧的 state 携带离散预计算值，不携带原始计数。
        [[nodiscard]] auto computeBotSilence(const json &snapshot, const size_t startIndex)
          -> std::pair<bool, size_t> {
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

        /// @brief Jev 三个问题的名称常量，与 action label 常量同处集中定义，避免字符串双写
        constexpr std::string_view kJevActionQuestion = "action";
        constexpr std::string_view kJevToneQuestion = "tone";
        constexpr std::string_view kJevMaxLengthQuestion = "maxLength";

        /// @brief 将单段消息内容渲染为 Jev 专用的单串文本 token
        /// @param segment MessageRecord::projectForAgent() 产出的段
        /// @details 渲染词表与 route() 分支里的 action criteria 措辞属于同一份契约，两者同时冻结。
        ///          未知段类型直接丢弃，渲染继续。
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
                return fmt::format("@{}", name.empty() ? getStr(target, "qq") : name);
            }
            if (type == "poke") {
                const json &target = atOrNull(segment, "target");
                const std::string name = getStr(target, "name");
                return fmt::format("[戳一戳:{}]", name.empty() ? getStr(target, "qq") : name);
            }
            if (type == "member_event") {
                return fmt::format("[群事件:{}]", getStr(segment, "action"));
            }
            if (type == "unsupported") {
                return fmt::format("[不支持:{}]", getStr(segment, "segment_type"));
            }
            return "";
        }

        /// @brief 构建 Jev 专用的扁平化记录
        /// @param message 已解析的持久化聊天记录
        /// @return 扁平化记录：{sender, text, is_current}
        /// @details 不复用 compactMessage() 的嵌套投影：去掉 message_id（顶层已有
        ///          trigger_message_id，记录上已有 is_current，足以定位），去掉
        ///          content.segments 嵌套结构，改为单串 text。机器人 sender 写显式标记
        ///          [bot]<botName>，其余用投影后的 sender.name，缺失时退回 QQ 号。
        ///          buildJevQuestions() 里“机器人刚发言且新消息非针对它”这条 criteria 依赖这一
        ///          区分，两处名字同源，均取 Config::instance().botName。
        [[nodiscard]] auto renderJevRecord(const json &message) -> json {
            const json projected = MessageRecord::projectForAgent(message);
            std::string text;
            for (const json &segment: atOrNull(projected, "segments")) {
                text += renderJevSegment(segment);
            }

            json record;
            if (MessageRecord::isAssistant(message)) {
                record["sender"] = fmt::format("[bot]{}", Config::instance().botName);
            } else if (const std::string name = getStr(atOrNull(projected, "sender"), "name"); !name.empty()) {
                record["sender"] = name;
            } else {
                record["sender"] = getStr(atOrNull(projected, "sender"), "qq");
            }
            record["text"] = std::move(text);
            return record;
        }

        /// @brief 构建 Jev 的 state：复用与兜底路径相同的窗口投影，产出裸 JSON 而非
        ///        role/content 数组
        /// @details 与 buildPrompt 共用 windowStartIndex 与 computeBotSilence，保证优先路径与
        ///          兜底路径看到同一对话窗口。触发消息标记沿用 is_current 键名。
        [[nodiscard]] auto buildJevState(const std::string_view triggerMessageId, const json &snapshot) -> json {
            const size_t startIndex = windowStartIndex(snapshot.size());
            const auto [spokeInWindow, silentCount] = computeBotSilence(snapshot, startIndex);

            json records = json::array();
            for (const json &message: snapshot | std::views::drop(startIndex)) {
                json record = renderJevRecord(message);
                if (getStr(message, "message_id") == triggerMessageId) {
                    record["is_current"] = true;
                }
                records.push_back(std::move(record));
            }

            json state;
            state["trigger_message_id"] = triggerMessageId;
            state["chat_records"] = std::move(records);
            state["bot_silence"] = {{"spoke_in_window", spokeInWindow},
              {"messages_since_last_speak", spokeInWindow ? silentCount : snapshot.size() - startIndex}};
            return state;
        }

        /// @brief 构建 Jev 的 questions：一次请求同时问三件事（是否回复 / 语气 / 最大长度）
        /// @details instructions 用英文书写，criteria 用中文（maintainer 裁定 1）。
        ///          action 的 unclear 置于映射末位，作为弃权出口。criteria 的每一句措辞在
        ///          renderJevSegment() 的渲染词表里都有对应 token，两者同时冻结。
        [[nodiscard]] auto buildJevQuestions() -> json {
            json action;
            action["type"] = "choice";
            action["instructions"] =
              "This is a group chat log. The record marked is_current is the message under evaluation. "
              "Decide whether the chat bot should reply to that message.";
            // criteria 里的机器人名与 renderJevRecord() 写入的 [bot]<botName> 逐字一致，
            // 统一从配置插值，不硬编码；botName 默认值是“机器人”。
            const std::string &botName = Config::instance().botName;
            action["criteria"] = {
              {"skip",
                fmt::format("纯表情/贴纸、单字或复读、广告、群友互聊且未涉及{0}、"
                            "{0}刚发言且新消息非针对它、无意义闲聊",
                  botName)},
              {"reply",
                fmt::format("被@或叫到名字、有提问、分享{0}可能感兴趣的内容、对{0}上次发言的反馈、"
                            "有梗可发挥、{0}久未发言或冷场需活跃",
                  botName)},
              {"unclear", "以上都不明显适用，或聊天记录不足以判断"},
            };

            json tone;
            tone["type"] = "choice";
            tone["instructions"] = "If the bot should reply, what tone should it use?";
            tone["criteria"] = {{"friendly", nullptr}, {"serious", nullptr}, {"casual", nullptr}};

            json maxLength;
            maxLength["type"] = "choice";
            maxLength["instructions"] = "If the bot should reply, what is the appropriate maximum reply length?";
            maxLength["criteria"] = {{"15", nullptr}, {"25", nullptr}, {"50", nullptr}, {"100", nullptr},
              {"200", nullptr}, {"500", nullptr}};

            return json{{std::string(kJevActionQuestion), std::move(action)},
              {std::string(kJevToneQuestion), std::move(tone)}, {std::string(kJevMaxLengthQuestion), std::move(maxLength)}};
        }

        [[nodiscard]] auto buildPrompt(
          const u64 sessionId, const std::string_view triggerMessageId, const json &snapshot) -> json {
            // 窗口起始下标计算已抽取为具名函数 windowStartIndex()，语义与产出逐字节不变。
            const size_t startIndex = windowStartIndex(snapshot.size());

            json records = json::array();
            for (const json &message: snapshot | std::views::drop(startIndex)) {
                json record = compactMessage(message);
                if (getStr(message, "message_id") == triggerMessageId) {
                    record["is_current"] = true;
                }
                records.push_back(std::move(record));
            }

            // bot_silence 计算已抽取为共享纯函数 computeBotSilence()，与 Jev 侧的 buildJevState()
            // 共用，本函数产出保持逐字节不变。
            const auto [spokeInWindow, silentCount] = computeBotSilence(snapshot, startIndex);

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

        /// @brief 由 Jev 判定构造决策
        /// @param jevResponse Jev 响应 JSON
        /// @param action Jev action choice 映射出的路由动作（REPLY 或 SKIP）
        /// @details action=unclear 不进入本函数，调用处已转入兜底。
        ///          action=skip 时 tone/maxLength 无语义，保留默认值。
        ///          action=reply 时把 tone 映射进 decision.tone，把 maxLength label 解析为整数并
        ///          clamp 到 [10, 500]，与 parseDecision 一致。maxLength 取值不为 0；该值写入
        ///          Executor 的 response_requirements.max_length。
        [[nodiscard]] auto makeJevDecision(const json &jevResponse, const RouterDecision::Action action)
          -> RouterDecision {
            RouterDecision decision = makeDecision(action, "Jev 优先判定");
            if (action != RouterDecision::Action::REPLY) {
                return decision; // skip 决策的 tone / maxLength 无语义，保留默认值
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

    auto classifyJevChoice(const std::string_view label) -> std::optional<RouterDecision::Action> {
        if (label == kJevActionReply) {
            return RouterDecision::Action::REPLY;
        }
        if (label == kJevActionSkip) {
            return RouterDecision::Action::SKIP;
        }
        // unclear 或其余任何未知取值（含空串）均视为弃权，交由 LLM 兜底判定。
        return std::nullopt;
    }

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

        // Jev 优先路径：仅群聊生效，位置在全部 5 条硬规则之后。硬规则先行保证：@提及与
        // 系统定时任务保留自身的 maxLength=100/isPriority=true 语义，机器人自己的消息不交给
        // Jev 判定，空快照不构造空 state。未配置 Jev 时行为与接入前一致。
        if (!SessionId::isPrivate(sessionId) && JevClient::isConfigured(config.jev)) {
            const auto jevResponse = co_await JevClient::requestSystemOne(config.jev,
              buildJevState(triggerMessageId, snapshot), buildJevQuestions(), sessionId);
            if (jevResponse) {
                const auto choice = JevClient::readChoice(
                  *jevResponse, std::string(kJevActionQuestion), {kJevActionSkip, kJevActionReply, kJevActionUnclear});
                const auto confidence = JevClient::readConfidence(*jevResponse, std::string(kJevActionQuestion));
                const auto action = choice ? classifyJevChoice(*choice) : std::nullopt;
                // 单行、key=value | 分隔、可 grep 可解析的日志，是观测 unclear 命中率的数据来源。
                // model 取响应回传的真实版本，不取配置中的别名。
                Logger::info(sessionId, "Jev",
                  fmt::format("choice={} | confidence={} | model={} | fallback={}", choice.value_or("n/a"),
                    confidence ? fmt::format("{:.4f}", *confidence) : "n/a",
                    getStr(*jevResponse, "model", config.jev.model), action ? "no" : "yes"));
                if (action) {
                    co_return applySessionType(makeJevDecision(*jevResponse, *action), sessionId);
                }
            } else {
                // 请求失败同样打一行 Jev 日志，用于统计失败导致的兜底率。
                Logger::info(sessionId, "Jev",
                  fmt::format("choice=n/a | confidence=n/a | model={} | fallback=yes", config.jev.model));
            }
        }

        // 兜底：现有 LLM 路由策略。保持 fail-open 语义不变：请求失败与响应无效两条路径
        // 都仍返回 REPLY。
        const auto response = std::optional{co_await LlmClient::requestChat("Router", "router", config.router,
          config.routerParams, buildPrompt(sessionId, triggerMessageId, snapshot), {}, sessionId)};
        if (!response) {
            co_return applySessionType(makeDecision(RouterDecision::Action::REPLY, "Router 请求失败"), sessionId);
        }
        co_return applySessionType(
          parseDecision(*response).value_or(makeDecision(RouterDecision::Action::REPLY, "Router 响应无效")), sessionId);
    }
} // namespace insoulforge::MessageRouter
