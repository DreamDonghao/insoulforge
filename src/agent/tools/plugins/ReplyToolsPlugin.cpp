/// @file ReplyToolsPlugin.cpp
/// @brief 回复工具插件实现（REPLY，调用即结束回合）
/// @details handler 为占位实现：回复工具在 ExecutorAgent::processToolCalls 内拦截执行，
///          不经 ToolRegistry::executeTool（需要改写回复决策而非返回工具结果）

#include <agent/tools/plugins/ReplyToolsPlugin.hpp>
#include <include/agent/tools/ToolRegistry.hpp>

namespace insoulforge {
    auto ReplyToolsPlugin::id() const noexcept -> std::string_view { return "builtin.reply"; }

    /// @brief 注册回复工具（REPLY，调用即结束回合）
    void ReplyToolsPlugin::registerTools(ToolRegistry &registry) const {

        // no_reply
        registry.registerTool(
          {
            .name = "no_reply",
            .description = "决定不回复消息。当：话题已参与过、没人问你、刚说过话、纯表情刷屏时使用。",
            .parameters = json(),
            .handler = [](json, ToolCallContext) -> drogon::Task<std::string> { co_return "ok"; },
          },
          ToolCategory::REPLY);

        // reply
        const json replyParams = json::parse(R"json({
            "type": "object",
            "properties": {
                "content": {
                    "type": "string",
                    "description": "要发送的回复内容"
                }
            },
            "required": ["content"]
        })json");
        registry.registerTool(
          {
            .name = "reply",
            .description = "回复消息。当：有人开启新的话题、有人问你、有人@你、有人求助时使用。",
            .parameters = replyParams,
            .handler = [](json, ToolCallContext) -> drogon::Task<std::string> { co_return "ok"; },
          },
          ToolCategory::REPLY);

        // reply_with_quote - 引用回复（REPLY，直接发送）
        const json quoteReplyParams = json::parse(R"json({
            "type": "object",
            "properties": {
                "content": {
                    "type": "string",
                    "description": "要发送的回复内容"
                },
                "message_id": {
                    "type": "string",
                    "description": "要引用的消息ID（从聊天记录JSON的message_id字段获取）"
                }
            },
            "required": ["content", "message_id"]
        })json");
        registry.registerTool(
          {
            .name = "reply_with_quote",
            .description = "引用回复特定消息。当需要回复特定消息、回答特定问题、澄清上下文时使用。聊"
                           "天记录格式为JSON：{\"message_id\":\"12345\",\"text\":\"...\"}，用 "
                           "message_id 字段的值作为参数。这是回复工具，调用后直接发送。",
            .parameters = quoteReplyParams,
            .handler = [](json, ToolCallContext) -> drogon::Task<std::string> { co_return "ok"; },
          },
          ToolCategory::REPLY);
    }

} // namespace insoulforge
