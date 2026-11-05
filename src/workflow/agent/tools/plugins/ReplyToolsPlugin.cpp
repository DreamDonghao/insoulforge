/// @file ReplyToolsPlugin.cpp
/// @brief 回复工具插件实现（REPLY，直接结束或发送成功后结束回合）
/// @details handler 为占位实现：回复工具在 ExecutorAgent::processToolCalls 内拦截执行，
///          不经 ToolRegistry::executeTool（需要改写回复决策而非返回工具结果）

#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/plugins/ReplyToolsPlugin.hpp>

namespace insoulforge {
    auto ReplyToolsPlugin::id() const noexcept -> std::string_view { return "builtin.reply"; }

    /// @brief 注册回复工具（REPLY，直接结束或发送成功后结束回合）
    void ReplyToolsPlugin::registerTools(ToolRegistry &registry) const {

        registry.registerTool({.name = "finish_turn",
          .description = "结束本轮处理，不发送消息。没有待处理事项或决定不再发送时使用。",
          .parameters = json(),
          .handler = [](json, ToolCallContext) -> drogon::Task<std::string> { co_return "ok"; }}, ToolCategory::REPLY);
        registry.registerTool({.name = "send_message_and_finish",
          .description = "向当前会话发送消息，确认发送成功后结束本轮。失败或结果未知时返回错误，由模型决定重发、继续处理或结束。",
          .parameters = {{"type", "object"}, {"properties", {
            {"content", {{"type", "string"}, {"description", "非空消息正文"}}},
            {"quote_message_id", {{"type", "string"}, {"description", "可选，当前会话聊天记录的 message_id；省略则普通发送"}}}}}, {"required", {"content"}}},
          .handler = [](json, ToolCallContext) -> drogon::Task<std::string> { co_return "ok"; }}, ToolCategory::REPLY);
    }
} // namespace insoulforge
