/// @file ExecutorAgent.cpp
/// @brief Executor Agent - 实现

#include <map>
#include <memory>
#include <stdexcept>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/activity/ActivityManager.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/llm/prompts/ExecutorSystemPrompt.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolArgument.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/agent/tools/mcp/McpTools.hpp>
#include <workflow/history/SharedConversationContext.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::ExecutorAgent {
    namespace {
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
            if (i >= text.size()) {
                return text;
            }
            return text.substr(0, i) + "…";
        }

        /// @brief 将冻结的完整消息投影为 Agent 上下文，可选截断较早消息的文本段
        [[nodiscard]] auto projectRecordForAgent(const json &raw, const bool truncateText, const bool includeGroupCard)
          -> json {
            if (!raw.is_object()) {
                return raw.is_string() ? raw : json(""); // 兼容纯文本记录
            }
            json content = MessageRecord::projectForAgent(raw, true, includeGroupCard);
            if (content.contains("memories")) {
                auto recalled = std::move(content["memories"]);
                content.erase("memories");
                content["recalled_memories"] = std::move(recalled);
            }
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
        /// @details 跳过机器人身份标记；映射中不存在的用户按 0（中立）注入
        [[nodiscard]] auto injectAffinity(json content, const std::unordered_map<UserId, i32> &affinityMap) -> json {
            if (!content.is_object() || !content.contains("sender")) {
                return content;
            }
            if (const auto userId = UserId::parse(getStr(content["sender"], "user_id"));
              userId && userId->toString() != "self") {
                const auto it = affinityMap.find(*userId);
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
        /// 好感度开启时为 sender 注入当前分数；消息预处理阶段已将召回记忆写入完整消息。
        auto buildChatContext(const ChatRecordManager &chatRecords) -> ChatContext {
            const auto &records = chatRecords.getRecords(); // 旧 → 新
            const size_t totalRecords = records.size();
            const size_t olderCount = totalRecords > kRecentRecordCount ? totalRecords - kRecentRecordCount : 0;

            const bool affinityEnabled = Config::instance().affinityEnabled.load(std::memory_order_relaxed);
            const auto affinityMap =
              affinityEnabled ? AffinityStore::getAffinityMap() : std::unordered_map<UserId, i32>{};

            const bool includeGroupCard = !chatRecords.getSessionId().isPrivate() &&
                                          SessionStore::getSessionConfig(chatRecords.getSessionId()).sendGroupCard;
            ChatContext context;

            for (size_t index = 0; index < totalRecords; ++index) {
                const bool isOlder = index < olderCount;
                auto &target = isOlder ? context.earlier : context.recent;
                auto record = projectRecordForAgent(records[index], isOlder, includeGroupCard);
                target.push_back(affinityEnabled ? injectAffinity(std::move(record), affinityMap) : std::move(record));
            }
            return context;
        }

        /// @brief 构建 Executor 消息列表（包含收藏表情的单条 system + 单条 user）
        /// @details 上下文组装为键序固定的单个 JSON 对象（ordered_json 按插入顺序序列化）作为单条
        /// user 消息：current_session → session_memory → conversation_summaries → earlier_messages → person_memories →
        /// shared_conversations → recent_messages → response_requirements（仅有 Router 策略时）→
        /// activities，避免连续多条 user（部分 OpenAI 兼容后端不支持）
        [[nodiscard]] auto buildPrompt(const ChatRecordManager &chatRecords, const MemoryManager &memory,
          const std::optional<RouterDecision> &decision) -> json {
            json messages = json::array();

            json systemMsg;
            systemMsg["role"] = "system";
            systemMsg["content"] = ExecutorSystemPrompt::instance().build(chatRecords.getSessionId());
            messages.push_back(systemMsg);

            auto [earlierRecords, recentRecords] = buildChatContext(chatRecords);

            json context;
            // 会话详情放最前：群名/群号供模型直接取用，无需再向工具查询
            const SessionId sessionId = chatRecords.getSessionId();
            json sessionInfo;
            if (sessionId.isPrivate()) {
                sessionInfo["type"] = "private";
                sessionInfo["user_id"] = sessionId.targetId();
            } else {
                sessionInfo["type"] = "group";
                sessionInfo["group_id"] = sessionId.targetId();
                if (const std::string groupName = SessionStore::getSessionName(sessionId); !groupName.empty()) {
                    sessionInfo["group_name"] = groupName;
                }
            }
            context["current_session"] = std::move(sessionInfo);
            const auto sessionMemory = memory.getMemory();
            context["session_memory"] = MemoryStore::projectShortTermMemory(sessionMemory);
            context["conversation_summaries"] = MemoryStore::conversationSummaries(sessionMemory);
            context["earlier_messages"] = std::move(earlierRecords);
            auto personMemories = PersonMemoryStore::getForSenders(recentRecords);
            context["person_memories"] = std::move(personMemories);
            context["shared_conversations"] = SharedConversationContext::build(
              sessionId, Config::instance().sharedConversationMessageCount.load(std::memory_order_relaxed));
            context["recent_messages"] = std::move(recentRecords);
            if (decision) {
                context["response_requirements"] = {{"tone", decision->tone}};
                if (decision->maxLength) {
                    context["response_requirements"]["max_length"] = *decision->maxLength;
                }
                context["response_requirements"]["reply_reason"] = decision->reason;
            }
            if (auto activities = ActivityManager::instance().activeActivities(); !activities.empty()) {
                context["activities"] = std::move(activities);
            }

            json userMsg;
            userMsg["role"] = "user";
            MessageRecord::compactContextUserIds(context);
            userMsg["content"] = dumpJson(context);
            messages.push_back(userMsg);

            return messages;
        }

        /// @brief 结果为内联 CQ 码、需在产出 reply 时自动拼入正文的工具（send_sticker 已直接发送，不经此路径）
        [[nodiscard]] auto isCqCodeTool(const std::string &name) -> bool { return name == "send_face"; }

        /// @brief 清理回复内容，并在模型忘记拼接 CQ 码时自动补上已获取的 CQ 码
        [[nodiscard]] auto finalizeContent(const std::string &rawContent, const std::string &accumulatedCQCodes)
          -> std::string {
            std::string content = cleanReplyContent(rawContent);
            if (!accumulatedCQCodes.empty() && content.find("[CQ:") == std::string::npos) {
                content += accumulatedCQCodes;
            }
            return content;
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
            if (rawArguments.is_object()) {
                return rawArguments;
            }
            json args;
            if (rawArguments.is_string()) {
                std::ignore = tryParseJson(jsonToString(rawArguments), args);
            }
            return args.is_object() ? args : json::object();
        }

        /// @brief 将工具参数序列化为 OpenAI tool 消息要求的 JSON 字符串
        [[nodiscard]] auto serializeToolArguments(const json &toolCall) -> std::string {
            const json &rawArguments = atOrNull(atOrNull(toolCall, "function"), "arguments");
            if (rawArguments.is_string()) {
                return jsonToString(rawArguments);
            }
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
        /// @return {回复决策, 工具结果消息列表, 累积 CQ 码, 按顺序保存的工具历史}
        auto processToolCalls(json message, json messages, std::string accumulatedCQCodes, const SessionId sessionId,
          const json &messageSnapshot, std::shared_ptr<std::vector<Image>> toolImages, const bool replyOnly,
          json toolHistory, std::shared_ptr<McpToolSelection> mcpSelection, std::map<std::string, u64> offeredMcpTools,
          UserId callerUserId) -> drogon::Task<std::tuple<std::optional<ReplyDecision>, json, std::string, json>> {
            ReplyDecision decision;
            bool hasDecision = false;

            for (const auto &toolCall: message["tool_calls"]) {
                const std::string name = jsonToString(atOrNull(atOrNull(toolCall, "function"), "name"));
                Logger::info(sessionId, "Executor", fmt::format("工具: {}", name));

                if (hasDecision) {
                    appendToolResult(messages, toolCall, "回合已结束，未执行: " + name);
                    continue;
                }
                const json args = parseToolArguments(toolCall);
                const auto recordCall = [&](const std::string &status) -> void {
                    // 正文和引用目标已保存在助手消息中，不重复记录发送内容的回复工具。
                    if (name == "send_message" || name == "send_message_and_finish") {
                        return;
                    }
                    toolHistory.push_back(MessageRecord::createToolHistoryEntry(name, args, status));
                };

                if (ToolRegistry::instance().isToolDisabled(name)) {
                    recordCall("rejected");
                    appendToolResult(messages, toolCall, "工具已关闭，未执行: " + name);
                    continue;
                }
                if (name == "finish_turn") {
                    decision.shouldReply = false;
                    recordCall("decision");
                    hasDecision = true;
                    continue;
                }
                if (name == "send_message_and_finish") {
                    auto content = finalizeContent(argString(args, "content"), accumulatedCQCodes);
                    if (content.empty()) {
                        appendToolResult(messages, toolCall, "content 必须是非空消息正文");
                        continue;
                    }
                    const auto quote = argString(args, "quote_message_id");
                    if (!quote.empty()) {
                        bool found = false;
                        for (const auto &record: messageSnapshot) {
                            if (getStr(record, "message_id") == quote) {
                                found = true;
                            }
                        }
                        if (!found) {
                            appendToolResult(messages, toolCall, "quote_message_id 必须来自当前会话可见消息");
                            continue;
                        }
                        content = "[CQ:reply,id=" + quote + "]" + content;
                    }
                    const auto sent =
                      co_await MessageService::sendToSession(sessionId, content, std::nullopt, toolHistory);
                    accumulatedCQCodes.clear();
                    if (!sent) {
                        recordCall("send_failed");
                        appendToolResult(messages, toolCall,
                          "发送失败或结果未知：" + sent.error() +
                            "。本轮未结束，由你决定重发、继续处理或 finish_turn；超时不代表确定未送达。");
                        continue;
                    }
                    recordCall("succeeded");
                    decision.messageSent = true;
                    decision.shouldReply = false;
                    hasDecision = true;
                    continue;
                }

                ToolCallContext ctx;
                ctx.sessionId = sessionId;
                ctx.callerUserId = callerUserId;
                ctx.messageSnapshot = messageSnapshot;
                ctx.toolImages = toolImages;
                ctx.replyOnly = replyOnly;
                ctx.mcpSelection = mcpSelection;
                ctx.offeredMcpTools = offeredMcpTools;

                const std::string result = co_await ToolRegistry::instance().executeTool(name, args, std::move(ctx));
                Logger::debug(sessionId, "Executor", fmt::format("工具结果: {}", result));
                recordCall(replyOnly ? "rejected" : "returned");
                if (!replyOnly && isCqCodeTool(name)) {
                    accumulatedCQCodes += result;
                }

                appendToolResult(messages, toolCall, result);
            }

            if (hasDecision) {
                co_return {decision, std::move(messages), std::move(accumulatedCQCodes), std::move(toolHistory)};
            }
            co_return {std::nullopt, std::move(messages), std::move(accumulatedCQCodes), std::move(toolHistory)};
        }

        /// @brief Agent 模式执行（带 tools）：循环「请求模型 → 处理工具调用」，直到产出回复决策或达最大轮数
        auto executeWithAgent(json messages, const SessionId sessionId, const json &messageSnapshot,
          std::shared_ptr<McpToolSelection> mcpSelection, const i32 maxToolRounds, UserId callerUserId)
          -> drogon::Task<std::optional<ReplyDecision>> {
            const auto &config = Config::instance();
            const auto isPrivateSession = sessionId.isPrivate();
            const json replyTools =
              ToolRegistry::instance().getTools({.isPrivateSession = isPrivateSession, .replyOnly = true});

            std::string accumulatedCQCodes; // 跨轮累积 CQ 码，产出 reply 时自动拼入正文
            json toolHistory = json::array();
            const auto toolImages = std::make_shared<std::vector<Image>>();
            const auto failedDecision = [&](std::string reason) -> std::optional<ReplyDecision> {
                if (toolHistory.empty()) {
                    return std::nullopt;
                }
                ReplyDecision decision;
                decision.toolHistory = std::move(toolHistory);
                decision.failureReason = std::move(reason);
                return decision;
            };

            // 一次回复固定使用启动时的上限，后台保存只影响后续流程。
            for (i32 round = 0; round < maxToolRounds; ++round) {
                const bool replyOnly = round + 1 == maxToolRounds;
                const auto roundTools = replyOnly
                                          ? replyTools
                                          : ToolRegistry::instance().getTools({.isPrivateSession = isPrivateSession,
                                              .mcpTools = mcpSelection->enabled,
                                              .mcpManagement = !mcpSelection->services.empty()});
                auto offeredMcpTools = std::map<std::string, u64>{};
                for (const auto &tool: roundTools) {
                    const auto name = getStr(tool["function"], "name");
                    if (const auto selected = mcpSelection->enabled.find(name);
                      selected != mcpSelection->enabled.end()) {
                        offeredMcpTools.emplace(*selected);
                    }
                }
                if (!replyOnly) {
                    mcpSelection->enabled = offeredMcpTools;
                }
                if (roundTools.empty()) {
                    Logger::error(sessionId, "Executor", "最后一轮未注册可用的回复工具");
                    co_return failedDecision("最后一轮没有可用的回复工具");
                }
                json mcpState = nullptr;
                if (!mcpSelection->services.empty() && !replyOnly) {
                    mcpState = {{"page_size", mcpSelection->limits.pageSize},
                      {"max_per_enable", mcpSelection->limits.maxPerEnable},
                      {"max_per_reply", mcpSelection->limits.maxPerReply},
                      {"selected_count", mcpSelection->enabled.size()}};
                }
                // 每轮使用最新缓存，不执行网络查询，也不把活动状态冻结在首轮。
                if (messages.size() > 1 && getStr(messages[1], "role") == "user") {
                    auto context = json::parse(getStr(messages[1], "content"));
                    context.erase("activities");
                    const auto activities = ActivityManager::instance().activeActivities();
                    if (!activities.empty()) {
                        context["activities"] = activities;
                    }
                    messages[1]["content"] = dumpJson(context);
                }
                auto requestMessages = buildIterationMessages(messages, round + 1, maxToolRounds, std::move(mcpState));
                // 兜底只重试当前请求，不改变后续轮次的默认模型，也不重新执行工具。
                const bool fallbackConfigured = LlmClient::isConfigured(config.executorFallback);
                auto respJson =
                  co_await LlmClient::requestChat("LLM", "executor", config.executor, config.executorParams,
                    fallbackConfigured ? requestMessages : std::move(requestMessages), roundTools, sessionId);
                if (!respJson && fallbackConfigured) {
                    Logger::warn(sessionId, "Executor", "默认模型请求失败，本次请求使用兜底模型");
                    respJson =
                      co_await LlmClient::requestChat("ExecutorFallback", "executorFallback", config.executorFallback,
                        config.executorFallbackParams, std::move(requestMessages), roundTools, sessionId);
                }
                if (!respJson) {
                    co_return failedDecision("Executor 模型请求失败");
                }

                const json &message = atOrNull((*respJson)["choices"][0], "message");

                // 无工具调用：文本即回复；无文本视为不回复
                if (!message.contains("tool_calls") || !message["tool_calls"].is_array() ||
                    message["tool_calls"].empty()) {
                    ReplyDecision decision;
                    decision.toolHistory = std::move(toolHistory);
                    if (message.contains("content") && !message["content"].is_null()) {
                        decision.shouldReply = true;
                        decision.content = cleanReplyContent(jsonToString(message["content"]));
                    }
                    co_return decision;
                }

                // 有工具调用：回传 assistant 消息后逐个处理，未命中回复工具则继续下一轮
                messages.push_back(buildAssistantToolCallMessage(message));
                auto [roundDecision, nextMessages, nextAccumulatedCQCodes, nextToolHistory] = co_await processToolCalls(
                  message, std::move(messages), std::move(accumulatedCQCodes), sessionId, messageSnapshot, toolImages,
                  replyOnly, std::move(toolHistory), mcpSelection, std::move(offeredMcpTools), callerUserId);
                if (roundDecision) {
                    roundDecision->toolHistory = std::move(nextToolHistory);
                    co_return std::move(roundDecision);
                }
                messages = std::move(nextMessages);
                accumulatedCQCodes = std::move(nextAccumulatedCQCodes);
                toolHistory = std::move(nextToolHistory);
            }

            Logger::error(sessionId, "Executor", fmt::format("达到最大迭代次数: {}", maxToolRounds));
            co_return failedDecision("达到最大迭代轮数，未生成最终回复");
        }

    } // namespace

    auto buildSystemPrompt(const SessionId &sessionId) -> std::string {
        return ExecutorSystemPrompt::instance().build(sessionId);
    }

    auto buildIterationMessages(json messages, const i32 currentRound, const i32 maxToolRounds, json mcpState) -> json {
        if (!messages.is_array() || currentRound < 1 || currentRound > maxToolRounds) {
            throw std::invalid_argument("执行轮次必须在 1 到总轮数之间，消息必须是数组");
        }
        json state = {{"current_round", currentRound}, {"max_rounds", maxToolRounds},
          {"remaining_rounds", maxToolRounds - currentRound}, {"reply_only", currentRound == maxToolRounds},
          {"current_time", currentDateTime()}};
        if (!mcpState.is_null() && currentRound != maxToolRounds) {
            state["mcp"] = std::move(mcpState);
        }
        const auto user =
          std::ranges::find_if(messages, [](const json &message) -> bool { return getStr(message, "role") == "user"; });
        if (user == messages.end() || !(*user)["content"].is_string()) {
            throw std::invalid_argument("缺少结构化 user 上下文");
        }
        auto context = json::parse((*user)["content"].get<std::string>());
        if (!context.is_object()) {
            throw std::invalid_argument("user 上下文必须是 JSON 对象");
        }
        context.erase("execution_state");
        context["execution_state"] = std::move(state);
        (*user)["content"] = dumpJson(context);
        return messages;
    }

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
          {std::regex(
             R"((send_message_and_finish|send_message|finish_turn|reply_with_quote|reply|no_reply)\s*\([^)]*\))"),
            ""},
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

    auto execute(const ChatRecordManager &chatRecords, const MemoryManager &memory,
      std::optional<RouterDecision> decision, std::string triggerMessageId)
      -> drogon::Task<std::optional<ReplyDecision>> {
        const SessionId sessionId = chatRecords.getSessionId();
        if (decision) {
            Logger::info(sessionId, "Executor",
              fmt::format("开始执行 | priority={} | maxLength={}", decision->isPriority,
                decision->maxLength ? std::to_string(*decision->maxLength) : "未指定"));
        } else {
            Logger::info(sessionId, "Executor", "开始执行 | 无 Router 回复策略");
        }

        const auto executionConfig = Config::instance().getExecutionConfig();
        auto mcpSelection = McpTools::instance().createSelection(executionConfig);
        try {
            // 首次使用或显式失效时刷新；成功结果由表情模块主动发布给 Builder。
            co_await ToolRuntime::fetchFavoriteEmojis(sessionId);
        } catch (const std::exception &error) {
            Logger::warn(sessionId, "Executor", fmt::format("收藏表情列表获取失败: {}", error.what()));
        }
        json messages = buildPrompt(chatRecords, memory, decision);

        auto callerUserId = UserId{"system"};
        for (const auto &record: chatRecords.getRecords()) {
            if (getStr(record, "message_id") == triggerMessageId) {
                if (const auto user = UserId::parse(getStr(atOrNull(record, "sender"), "user_id")); user) {
                    callerUserId = *user;
                }
                break;
            }
        }
        co_return co_await executeWithAgent(std::move(messages), sessionId, chatRecords.getRecords(),
          std::move(mcpSelection), getInt(executionConfig, "maxToolRounds", 8), callerUserId);
    }
} // namespace insoulforge::ExecutorAgent
