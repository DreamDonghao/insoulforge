/// @file InfoToolsPlugin.cpp
/// @brief 信息工具插件实现（INFORMATION，查询数据、获取答案，不产生副作用）

#include <include/agent/tools/ToolRegistry.hpp>
#include <agent/tools/ToolArgument.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/plugins/InfoToolsPlugin.hpp>
#include <algorithm>
#include <infrastructure/config/Config.hpp>
#include <fmt/core.h>
#include <conversation/message/SessionId.hpp>
#include <llm/LlmClient.hpp>
#include <agent/memory/LongTermMemory.hpp>
#include <agent/ability/TaskStore.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        /// @brief 日志中深度思考结果的预览长度
        constexpr size_t kThinkingPreviewChars = 100;

        /// @brief 深度思考的系统提示词（专家求解定位：只产出问题答案，不组织聊天回复）
        std::string buildThinkingSystemPrompt(const std::string &question) {
            return fmt::format("你是被咨询的领域专家，负责解答问题、提供知识与分析。\n\n"
                               "结合对话上下文理解【问题】，给出准确的答案。\n\n"
                               "【问题】\n{}\n\n"
                               "【输出要求】\n"
                               "- 只输出针对问题的分析、推理与答案本身\n"
                               "- 不组织聊天回复，不揣摩语气和长度\n"
                               "- 结论先行，过程简明\n"
                               "- 不使用 markdown 代码块",
              question.empty() ? "结合上下文分析最新消息，给出准确答案" : question);
        }

    } // namespace

    std::string_view InfoToolsPlugin::id() const noexcept { return "builtin.info"; }

    /// @brief 注册内容获取工具（INFORMATION，查询数据、获取答案，不产生副作用）
    void InfoToolsPlugin::registerTools(ToolRegistry &registry) const {

        // list_stickers
        registry.registerTool(
          {
            .name = "list_stickers",
            .description = "获取QQ收藏表情中所有可用的表情名称列表。",
            .parameters = json(),
            .handler = [](json, ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto sessionId = ctx.sessionId;
                const json emojis = co_await ToolRuntime::fetchFavoriteEmojis(sessionId);
                if (emojis.empty()) {
                    co_return std::string("表情库为空（QQ收藏表情列表获取失败或没有收藏表情）");
                }
                std::string result = "可用表情: ";
                bool first = true;
                for (const auto &emoji: emojis) {
                    if (!first)
                        result += ", ";
                    result += getStr(emoji, "name");
                    first = false;
                }
                co_return result;
            },
          },
          ToolCategory::INFORMATION);

        // recall_memory
        const json memoryParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "要回忆的内容关键词，如某人的喜好、某群的习惯等"
                    }
                },
                "required": ["query"]
            })json");
        registry.registerTool(
          {
            .name = "recall_memory",
            .description = "从长期记忆库中回忆信息。当想不起某人"
                           "喜好、某群习惯、过去的约定时使用。模拟人类回忆过程。",
            .parameters = memoryParams,
            .handler = [](const json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                const std::string query = argString(args, "query");
                if (query.empty())
                    co_return std::string("请提供回忆关键词");

                const uint64_t sessionId = ctx.sessionId;
                const auto result = co_await LongTermMemory::searchMemory(query, 3, sessionId);
                if (!result || result->empty()) {
                    co_return "想不起来了，没有找到相关记忆";
                }
                co_return "回忆起：" + result.value();
            },
          },
          ToolCategory::INFORMATION);

        // deep_think - 调用深度思考模型求解。上下文由 Executor 随 ToolCallContext 传入
        // （system 之后的完整消息列表，含已获取的工具结果）
        const json thinkParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "question": {
                        "type": "string",
                        "description": "用一两句话描述需要深入分析的具体问题”"
                    }
                },
                "required": ["question"]
            })json");
        registry.registerTool(
          {
            .name = "deep_think",
            .description = "深度思考工具。遇到复杂问题（数学计算、多步推理、逻辑分析、技术难题等）时调用："
                           "深度思考模型会结合当前对话上下文给出准确的答案或解法，"
                           "拿到答案后据此组织回复。简单闲聊、日常对话禁止调用。",
            .parameters = thinkParams,
            .handler = [](json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                const uint64_t sessionId = ctx.sessionId;
                const std::string question = argString(args, "question");

                // 上下文随调用参数传入，由协程帧持有，co_await 期间不会被其他会话覆盖
                json context = std::move(ctx.conversationContext);
                if (!context.is_array() || context.empty()) {
                    co_return std::string("会话上下文缺失，无法深度思考");
                }

                json messages = json::array();
                json systemMsg;
                systemMsg["role"] = "system";
                systemMsg["content"] = buildThinkingSystemPrompt(question);
                messages.push_back(std::move(systemMsg));
                for (const auto &msg: context) {
                    messages.push_back(msg);
                }

                const auto &config = Config::instance();
                const auto respJson = co_await LlmClient::requestChat("深度思考", "executorThinking",
                  config.executorThinking, config.executorThinkingParams, std::move(messages), {}, sessionId);
                if (!respJson) {
                    co_return std::string("深度思考暂时不可用，请直接根据已有信息回复");
                }

                // 答案在 content；个别思考模型异常时仅输出 reasoning_content，兜底取用
                const json &message = atOrNull((*respJson)["choices"][0], "message");
                std::string content;
                if (message.contains("content") && !message["content"].is_null()) {
                    content = jsonToString(message["content"]);
                } else if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
                    content = jsonToString(message["reasoning_content"]);
                }
                if (content.empty()) {
                    co_return std::string("深度思考暂时不可用，请直接根据已有信息回复");
                }

                Logger::session(sessionId).debug(
                  "[深度思考] 结果: {}...", content.substr(0, std::min(kThinkingPreviewChars, content.length())));
                co_return "【深度思考结果】\n" + content;
            },
          },
          ToolCategory::INFORMATION);

        // list_scheduled_tasks - 查看当前会话的定时任务（定时任务三件套中的信息工具）
        registry.registerTool(
          {
            .name = "list_scheduled_tasks",
            .description = "查看当前会话所有待触发的定时任务。当用户想确认已设置的提醒、或取消前需要获取任务编号"
                           "时使用。返回任务编号、触发时间和备忘内容。",
            .parameters = json(),
            .handler = [](json, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const uint64_t sessionId = ctx.sessionId;
                if (sessionId == 0)
                    co_return std::string("会话上下文缺失，无法查询定时任务");
                const auto [sessionType, targetId] = SessionId::toStorageTarget(sessionId);
                const auto tasks = TaskStore::getPendingScheduledTasksByTarget(sessionType, targetId);
                if (tasks.empty()) {
                    co_return std::string("当前会话没有待触发的定时任务");
                }

                std::string out = fmt::format("当前会话共有 {} 个待触发定时任务：\n", tasks.size());
                for (const auto &task: tasks) {
                    if (task.isDaily) {
                        out += fmt::format("- 任务#{}：每天 {} 「{}」（每日重复）\n", task.id,
                          formatTimeOfDay(task.remindTime), task.content);
                    } else {
                        out +=
                          fmt::format("- 任务#{}：{} 「{}」\n", task.id, formatUnixTime(task.remindTime), task.content);
                    }
                }
                out += "如需取消某个任务，调用 cancel_scheduled_task 并传入任务编号（#后的数字）";
                co_return out;
            },
          },
          ToolCategory::INFORMATION);
    }

} // namespace insoulforge
