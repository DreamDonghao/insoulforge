/// @file ExecutorAgent.cpp
/// @brief Executor Agent - 实现

#include <infrastructure/NumericTypes.hpp>

#include <agent/runtime/ExecutorAgent.hpp>
#include <conversation/maintenance/affinity/AffinityStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionStore.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
#include <llm/PromptService.hpp>

namespace insoulforge::ExecutorAgent {
    namespace {
        /// @brief 工具调用循环最大轮数（防止模型无限循环调用工具）
        constexpr i32 kMaxToolRounds = 8;

        /// @brief 获取系统提示词（私聊与群聊使用各自的人设提示词，差异行按会话类型拼接）
        auto getSystemPrompt(const RouterDecision &decision) -> std::string {
            std::string prompt = decision.isPrivate ? PromptService::getExecutorPrivateSystemPrompt()
                                                    : PromptService::getExecutorSystemPrompt();
            prompt +=
              R"(## 聊天记录格式说明

你收到的上下文是键顺序固定的 JSON，earlier_conversation 与 recent_conversation 均按时间从旧到新排列：

```json
{
    "short_term_memory": [
        "我已记住的本群信息，每条一项，可能为空数组"
    ],
    "earlier_conversation": [
        {
            "time": "消息时间，如 2026-09-02 19:59:54",
            "sender": {
                "name": "发送者昵称",
                "qq": "发送者QQ号",
                "affinity": 好感度（-100～100 的整数，仅群成员消息有此字段，陌生人为 0）
            },
            "message_id": "消息ID（数字字符串）",
            "segments": "按原始顺序保存的富内容段；图片段带 image_index、识别状态和描述",
            "reply_to": "此消息引用回复的唯一目标消息ID，仅实际引用时存在"
        }
    ],
    "recent_conversation": [
        {
            "time": "消息时间，如 2026-09-02 19:59:54",
            "sender": {
                "name": "发送者昵称",
                "qq": "发送者QQ号",
                "affinity": 好感度（-100～100 的整数，仅群成员消息有此字段，陌生人为 0）
            },
            "message_id": "消息ID（数字字符串）",
            "segments": "按原始顺序保存的富内容段；图片描述仅在图片段出现一次",
          	"memories": [
                "召回的记忆片段，可能无关",
            ],
            "reply_to": "此消息引用回复的唯一目标消息ID，仅实际引用时存在"
        }
    ],
    "response_requirements": {
        "tone": "本轮要求的回复语气",
        "max_length": 100,
        "reply_reason": "触发本轮回复的原因，如 用户@提及"
    }
}
```

  字段说明：

  - sender.qq 为 "self" 的记录是我自己发出的消息（name 形如「昵称(我)」），它没有 affinity 字段
  - segments 是消息内容和顺序的唯一来源：text、at、image、face、sticker、poke、member_event 都以类型化段表示
  - image 段的 image_index 从 0 开始；图片来源不会出现在上下文。用户要求保存图片为表情时，save_sticker 传该消息的 message_id、image_index 和名称
  - 需要引用回复某条消息时，把该记录的 message_id 传给 reply_with_quote
  - response_requirements.max_length 是本轮回复的字数上限，回复尽量不超过

## 工具调用规则

### 回复工具必选其一（三选一）在最后调用

| 工具              | 使用场景                                          |
| ---------------- | -------------------------------------------------|
| reply            | 普通回复                                          |
| reply_with_quote | 需引用特定消息（聊天记录有多个话题/回复历史消息）        |
| no_reply         | 无需回复文本内容                                    |

### 辅助工具

at_user、等 → 返回CQ码，嵌入 reply 的 content 参数中发送

### 调用机制

- 工具通过 function calling 自动执行
- 严禁在回复中写出「调用工具xxx(...)」
- 直接调用工具，回复内容放参数中
- reply 必须传入非空 content，例如 reply({"content":"你好"})；不要把最终回复只写在 assistant 的 content 字段
- 模型只决定调用哪个工具、传什么参数

表情/图片的CQ码必须通过工具获取(send_sticker/send_face/send_image)。
先调工具，拿到结果后把返回的[CQ:image...]或[CQ:face...]原样拼接到reply的content中。
禁止自己编造假CQ标签，工具返回什么就复制什么。
send_sticker/send_poke/reply_and_continue 都是中途动作：发出后回合不结束，最终仍要用 reply/no_reply 收尾。
send_sticker：表情包直接作为独立消息发出，不拼进reply；若表情包就是全部回复，发完调 no_reply 收尾

reply_and_continue：接下来要执行耗时操作（网络搜索、深度思考、查资料等用户需要等待的事）时，必须先用该工具提醒用户一下，
再调耗时工具，拿到结果后用 reply 给出最终回复；
操作失败也要 reply 告知结果，不能没有下文。想在正式回复前先发其他内容（连续多条消息）时也可以用它

**输入的聊天记录是JSON格式，但你调用回复工具时的内容必须是纯文本，不是JSON！**
要有自己的判断，不要别人说什么就做什么)";

            if (decision.isPrivate) {
                prompt += "- 这是私聊，直接回复即可，不要@对方或引用回复\n";
            } else {
                prompt += "- @人格式: @[QQ:123456]\n"
                          "- 禁言要核实实际情况再决定\n";
            }

            return prompt;
        }

        /// @brief 解析记录的 content 字段（由本服务写入的 JSON 对象字符串）
        [[nodiscard]] auto parseRecordContent(const json &record) -> json {
            json parsed;
            if (!tryParseJson(getStr(record, "content"), parsed) || !parsed.is_object()) {
                return {};
            }
            return parsed;
        }

        /// @brief 按 UTF-8 字符边界截断，超长时末尾追加省略号
        [[nodiscard]] auto truncateUtf8(const std::string &text, const size_t maxChars) -> std::string {
            size_t i = 0;
            size_t count = 0;
            while (i < text.size() && count < maxChars) {
                if (const auto c = static_cast<unsigned char>(text[i]); (c & 0xE0) == 0xC0) {
                    i += 2;
                } else if ((c & 0xF0) == 0xE0) {
                    i += 3;
                } else if ((c & 0xF8) == 0xF0) {
                    i += 4;
                } else {
                    i += 1;
                }
                ++count;
            }
            if (i >= text.size())
                return text;
            return text.substr(0, i) + "…";
        }

        /// @brief 将持久化记录投影为 Agent 上下文，可选截断较早消息的文本段
        [[nodiscard]] auto projectRecordForAgent(const json &record, const bool truncateText) -> json {
            const json raw = parseRecordContent(record);
            if (!raw.is_object()) {
                return getStr(record, "content"); // 历史存量可能是纯文本
            }
            json content = MessageRecord::projectForAgent(raw);
            if (!truncateText) {
                return content;
            }
            for (auto &segment: content["segments"]) {
                if (getStr(segment, "type") == "text") {
                    constexpr size_t kOldRecordMaxChars = 500;
                    segment["text"] = truncateUtf8(getStr(segment, "text"), kOldRecordMaxChars);
                }
            }
            return content;
        }

        /// @brief 注入发送者当前好感度（读时注入，保证 LLM 看到的永远是最新值）
        /// @details qq 非数字（机器人记录的 "self"）跳过；映射中不存在的用户按 0（中立）注入
        [[nodiscard]] auto injectAffinity(json content, const std::unordered_map<u64, i32> &affinityMap) -> json {
            if (!content.is_object() || !content.contains("sender"))
                return content;
            if (const u64 qq = parseUInt64(getStr(content["sender"], "qq")); qq > 0) {
                const auto it = affinityMap.find(qq);
                content["sender"]["affinity"] = it != affinityMap.end() ? it->second : 0;
            }
            return content;
        }

        /// @brief 聊天记录上下文（窗口内，旧 → 新）
        struct ChatContext {
            json earlier = json::array(); // 更早记录：文本段截断到 500 字
            json recent = json::array(); // 最近记录：注入好感度与召回的长期记忆
        };

        /// @brief 构建聊天记录上下文：
        /// 最新 kRecentRecordCount 条保留完整语义投影，更早记录的文本段截断到 500 字；
        /// 每条 sender 注入当前好感度 affinity；消息预处理阶段已将召回记忆写入完整消息。
        auto buildChatContext(const ChatRecordManager &chatRecords) -> ChatContext {
            const auto records = chatRecords.getRecords(); // 旧 → 新
            const size_t totalRecords = records.size();
            const size_t olderCount = totalRecords > kRecentRecordCount ? totalRecords - kRecentRecordCount : 0;
            const auto olderRecords = records | std::views::take(olderCount);
            const auto recentRecords = records | std::views::drop(olderCount);

            const auto affinityMap = AffinityStore::getAffinityMap(chatRecords.getSessionId());

            ChatContext context;

            // 处理更早的对话
            for (const auto &record: olderRecords) {
                context.earlier.push_back(injectAffinity(projectRecordForAgent(record, true), affinityMap));
            }

            // 处理最近对话（消息预处理时已注入召回记忆）
            for (const auto &record: recentRecords) {
                context.recent.push_back(injectAffinity(projectRecordForAgent(record, false), affinityMap));
            }
            return context;
        }

        /// @brief 把短期记忆文本（每行一条）拆为字符串数组，跳过空白行
        [[nodiscard]] auto splitMemoryLines(const std::string &text) -> json {
            json lines = json::array();
            for (size_t start = 0; start < text.size();) {
                const size_t end = text.find('\n', start);
                const size_t stop = end == std::string::npos ? text.size() : end;
                if (std::string line = trim(text.substr(start, stop - start)); !line.empty())
                    lines.push_back(std::move(line));
                start = stop + 1;
            }
            return lines;
        }

        /// @brief 构建 Executor 消息列表（system + 单条 user）
        /// @details 上下文组装为键序固定的单个 JSON 对象（ordered_json 按插入顺序序列化）作为单条
        /// user 消息：session → short_term_memory → earlier_conversation → recent_conversation →
        /// response_requirements，
        /// 避免连续多条 user（部分 OpenAI 兼容后端不支持）
        [[nodiscard]] auto buildPrompt(
          const ChatRecordManager &chatRecords, const MemoryManager &memory, const RouterDecision &decision) -> json {
            json messages = json::array();

            json systemMsg;
            systemMsg["role"] = "system";
            systemMsg["content"] = getSystemPrompt(decision);
            messages.push_back(systemMsg);

            auto [earlierRecords, recentRecords] = buildChatContext(chatRecords);

            json context;
            // 会话详情放最前：群名/群号供模型直接取用，无需再向工具查询
            const u64 sessionId = chatRecords.getSessionId();
            json sessionInfo;
            if (SessionId::isPrivate(sessionId)) {
                sessionInfo["type"] = "private";
                sessionInfo["qq"] = SessionId::toStorageTarget(sessionId).second;
            } else {
                sessionInfo["type"] = "group";
                sessionInfo["group_id"] = sessionId;
                if (const std::string groupName = SessionStore::getSessionName(sessionId); !groupName.empty()) {
                    sessionInfo["group_name"] = groupName;
                }
            }
            context["session"] = std::move(sessionInfo);
            context["short_term_memory"] = splitMemoryLines(memory.getMemory());
            context["earlier_conversation"] = std::move(earlierRecords);
            context["recent_conversation"] = std::move(recentRecords);
            json requirements;
            requirements["tone"] = decision.tone;
            requirements["max_length"] = decision.maxLength;
            requirements["reply_reason"] = decision.reason;
            context["response_requirements"] = std::move(requirements);

            json userMsg;
            userMsg["role"] = "user";
            userMsg["content"] = dumpJson(context);
            messages.push_back(userMsg);

            return messages;
        }

        /// @brief 结果为内联 CQ 码、需在产出 reply 时自动拼入正文的工具（send_sticker 已直接发送，不经此路径）
        [[nodiscard]] auto isCqCodeTool(const std::string &name) -> bool {
            return name == "send_face" || name == "send_image";
        }

        /// @brief 清理回复内容，并在模型忘记拼接 CQ 码时自动补上已获取的 CQ 码
        [[nodiscard]] auto finalizeContent(const std::string &rawContent, const std::string &accumulatedCQCodes)
          -> std::string {
            std::string content = cleanReplyContent(rawContent);
            if (!accumulatedCQCodes.empty() && content.find("[CQ:") == std::string::npos) {
                content += accumulatedCQCodes;
            }
            return content;
        }

        enum class ReplyToolResult {
            NotReplyTool,
            Applied,
            InvalidArguments,
        };

        /// @brief 校验并处理回复工具（no_reply / reply / reply_with_quote）
        /// @return 已生成决策、参数无效或非回复工具；参数无效由调用方回传错误给模型。
        [[nodiscard]] auto applyReplyTool(const std::string &name, const json &args,
          const std::string &accumulatedCQCodes, ReplyDecision &decision) -> ReplyToolResult {
            if (name == "no_reply") {
                decision.shouldReply = false;
                return ReplyToolResult::Applied;
            }
            if (name == "reply") {
                if (!args.is_object() || !args.contains("content"))
                    return ReplyToolResult::InvalidArguments;
                decision.content = finalizeContent(jsonToString(args["content"]), accumulatedCQCodes);
                if (decision.content.empty())
                    return ReplyToolResult::InvalidArguments;
                decision.shouldReply = true;
                return ReplyToolResult::Applied;
            }
            if (name == "reply_with_quote") {
                if (!args.is_object() || !args.contains("content") || !args.contains("message_id"))
                    return ReplyToolResult::InvalidArguments;
                const std::string messageId = jsonToString(args["message_id"]);
                const std::string content = finalizeContent(jsonToString(args["content"]), accumulatedCQCodes);
                if (messageId.empty() || content.empty())
                    return ReplyToolResult::InvalidArguments;
                decision.shouldReply = true;
                decision.content = fmt::format("[CQ:reply,id={}]", messageId) + content;
                return ReplyToolResult::Applied;
            }
            return ReplyToolResult::NotReplyTool;
        }

        /// @brief 向模型回传单个工具调用的执行结果或参数错误
        void appendToolResult(json &messages, const json &toolCall, std::string content) {
            json toolMsg;
            toolMsg["role"] = "tool";
            toolMsg["tool_call_id"] = jsonToString(atOrNull(toolCall, "id"));
            toolMsg["content"] = std::move(content);
            messages.push_back(std::move(toolMsg));
        }

        /// @brief 解析兼容接口返回的工具参数
        /// @details OpenAI 兼容格式使用 JSON 字符串，部分接口直接返回 JSON 对象；两种格式均归一化为对象。
        [[nodiscard]] auto parseToolArguments(const json &toolCall) -> json {
            const json &rawArguments = atOrNull(atOrNull(toolCall, "function"), "arguments");
            if (rawArguments.is_object())
                return rawArguments;
            json args;
            if (rawArguments.is_string()) {
                std::ignore = tryParseJson(jsonToString(rawArguments), args);
            }
            return args.is_object() ? args : json::object();
        }

        /// @brief 将工具参数序列化为 OpenAI tool 消息要求的 JSON 字符串
        [[nodiscard]] auto serializeToolArguments(const json &toolCall) -> std::string {
            const json &rawArguments = atOrNull(atOrNull(toolCall, "function"), "arguments");
            if (rawArguments.is_string())
                return jsonToString(rawArguments);
            return rawArguments.is_object() ? dumpJson(rawArguments) : "{}";
        }

        /// @brief 把模型返回的 tool_calls 转为需回传以补全上下文的 assistant 消息
        [[nodiscard]] auto buildAssistantToolCallMessage(const json &message) -> json {
            json assistantMsg;
            assistantMsg["role"] = "assistant";
            assistantMsg["content"] = message.contains("content") ? jsonToString(message["content"]) : "";

            json toolCalls = json::array();
            for (const auto &toolCall: message["tool_calls"]) {
                json entry;
                entry["id"] = jsonToString(atOrNull(toolCall, "id"));
                entry["type"] = "function";
                entry["function"]["name"] = jsonToString(atOrNull(atOrNull(toolCall, "function"), "name"));
                entry["function"]["arguments"] = serializeToolArguments(toolCall);
                toolCalls.push_back(entry);
            }
            assistantMsg["tool_calls"] = toolCalls;
            return assistantMsg;
        }

        /// @brief 逐个处理本轮工具调用：回复工具直接产出回复决策；其余工具经 ToolRegistry 执行并把结果
        /// 作为 tool 消息回传，CQ 码类工具的结果累积备用
        /// @return {回复工具决策（同轮多个以最后一个为准，未命中为 nullopt）, 回传工具结果后的消息列表, 累积的 CQ 码}
        auto processToolCalls(json message, json messages, std::string accumulatedCQCodes, const u64 sessionId,
          const json &messageSnapshot) -> drogon::Task<std::tuple<std::optional<ReplyDecision>, json, std::string>> {
            ReplyDecision decision;
            bool hasDecision = false;

            for (const auto &toolCall: message["tool_calls"]) {
                const std::string name = jsonToString(atOrNull(atOrNull(toolCall, "function"), "name"));
                Logger::info(sessionId, "Executor", fmt::format("工具: {}", name));

                const json args = parseToolArguments(toolCall);

                const ReplyToolResult replyResult = applyReplyTool(name, args, accumulatedCQCodes, decision);
                if (replyResult == ReplyToolResult::Applied) {
                    hasDecision = true; // 回复工具：结束本轮，不回传工具结果
                    continue;
                }
                if (replyResult == ReplyToolResult::InvalidArguments) {
                    // 部分兼容接口会把最终文本放在 assistant content，却返回 reply({})。
                    if (name == "reply") {
                        const std::string fallback =
                          finalizeContent(jsonToString(atOrNull(message, "content")), accumulatedCQCodes);
                        if (!fallback.empty()) {
                            Logger::warn(sessionId, "Executor", "reply 参数为空，使用 assistant content 兜底");
                            decision.shouldReply = true;
                            decision.content = fallback;
                            hasDecision = true;
                            continue;
                        }
                    }
                    const std::string error = name == "reply_with_quote"
                                                ? "reply_with_quote 需要非空 content 和 message_id，请重新调用。"
                                                : "reply 需要非空 content，请重新调用。";
                    Logger::warn(sessionId, "Executor", fmt::format("回复工具参数无效: {}", name));
                    appendToolResult(messages, toolCall, error);
                    continue;
                }

                // deep_think 以会话上下文为参考材料：快照 system 之后的完整消息列表（含已获取的工具结果）
                // 随调用传给 handler；其余工具不拷贝这份上下文
                ToolCallContext ctx;
                ctx.sessionId = sessionId;
                ctx.messageSnapshot = messageSnapshot;
                if (name == "deep_think") {
                    ctx.conversationContext = json::array();
                    for (size_t i = 1; i < messages.size(); ++i) {
                        ctx.conversationContext.push_back(messages[i]);
                    }
                }

                const std::string result = co_await ToolRegistry::instance().executeTool(name, args, std::move(ctx));
                Logger::debug(sessionId, "Executor", fmt::format("工具结果: {}", result));
                if (isCqCodeTool(name)) {
                    accumulatedCQCodes += result;
                }

                appendToolResult(messages, toolCall, result);
            }

            if (hasDecision) {
                co_return {decision, std::move(messages), std::move(accumulatedCQCodes)};
            }
            co_return {std::nullopt, std::move(messages), std::move(accumulatedCQCodes)};
        }

        /// @brief Agent 模式执行（带 tools）：循环「请求模型 → 处理工具调用」，直到产出回复决策或达最大轮数
        auto executeWithAgent(json messages, const u64 sessionId, const json &messageSnapshot)
          -> drogon::Task<std::optional<ReplyDecision>> {
            const auto &config = Config::instance();
            const json tools = ToolRegistry::instance().getTools({.isPrivateSession = SessionId::isPrivate(sessionId)});
            if (tools.empty()) {
                Logger::error(sessionId, "Executor", "未注册工具");
                co_return std::nullopt;
            }

            std::string accumulatedCQCodes; // 跨轮累积 CQ 码，产出 reply 时自动拼入正文

            for (i32 round = 0; round < kMaxToolRounds; ++round) {
                const auto respJson = co_await LlmClient::requestChat(
                  "LLM", "executor", config.executor, config.executorParams, messages, tools, sessionId);
                if (!respJson) {
                    co_return std::nullopt;
                }

                const json &message = atOrNull((*respJson)["choices"][0], "message");

                // 无工具调用：文本即回复；无文本视为不回复
                if (!message.contains("tool_calls") || !message["tool_calls"].is_array() ||
                    message["tool_calls"].empty()) {
                    ReplyDecision decision;
                    if (message.contains("content") && !message["content"].is_null()) {
                        decision.shouldReply = true;
                        decision.content = cleanReplyContent(jsonToString(message["content"]));
                    }
                    co_return decision;
                }

                // 有工具调用：回传 assistant 消息后逐个处理，未命中回复工具则继续下一轮
                messages.push_back(buildAssistantToolCallMessage(message));
                auto [roundDecision, nextMessages, nextAccumulatedCQCodes] = co_await processToolCalls(
                  message, std::move(messages), std::move(accumulatedCQCodes), sessionId, messageSnapshot);
                if (roundDecision) {
                    co_return std::move(roundDecision);
                }
                messages = std::move(nextMessages);
                accumulatedCQCodes = std::move(nextAccumulatedCQCodes);
            }

            Logger::error(sessionId, "Executor", "达到最大迭代次数");
            co_return std::nullopt;
        }

    } // namespace

    /// @brief 清理模型输出的污染内容（think标签、tool_call标签、DSML标签等）
    auto cleanReplyContent(const std::string &text) -> std::string {
        // 按序应用的净化规则：Qwen 的 DSML 标签、DeepSeek 的 think 标签、tool_call 残留等
        static const std::vector<std::pair<std::regex, std::string>> rules = {
          // <tool_call>...</tool_call> 块及残留的独立标签
          {std::regex("<tool_call>[^<]*</tool_call>", std::regex::icase), ""},
          {std::regex("</?tool_call>", std::regex::icase), ""},
          // DSML 工具调用块（兼容全角竖线｜）及残留标签
          {std::regex(R"(<[|｜]*DSML[|｜]+invoke[^>]*>[\s\S]*?<\/[|｜]*DSML[|｜]+invoke>)", std::regex::icase), ""},
          {std::regex(R"(<\/?[|｜]*DSML[|｜]+[^>]*>)", std::regex::icase), ""},
          // </think> 与 function(...) 调用残留
          {std::regex("</think>", std::regex::icase), ""},
          {std::regex(R"((reply_with_quote|reply|no_reply)\s*\([^)]*\))"), ""},
          // 压缩空白：去除首尾空白与 3 个以上连续换行
          {std::regex("^\\s+|\\s+$"), ""},
          {std::regex("\n{3,}"), "\n\n"},
        };

        std::string result = text;
        for (const auto &[pattern, replacement]: rules) {
            result = std::regex_replace(result, pattern, replacement);
        }

        // 清理后为空但原文非空：保留原文，宁可原样输出也不发空消息
        if (result.empty() && !text.empty()) {
            return text;
        }
        return result;
    }

    auto execute(const ChatRecordManager &chatRecords, const MemoryManager &memory, RouterDecision decision,
      const json messageSnapshot) -> drogon::Task<std::optional<ReplyDecision>> {
        const u64 sessionId = chatRecords.getSessionId();
        Logger::info(sessionId, "Executor",
          fmt::format("开始执行 | priority={} | maxLength={}", decision.isPriority, decision.maxLength));

        json messages = buildPrompt(chatRecords, memory, decision);

        co_return co_await executeWithAgent(std::move(messages), sessionId, messageSnapshot);
    }
} // namespace insoulforge::ExecutorAgent
