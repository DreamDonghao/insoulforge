/// @file ExecutorAgent.hpp
/// @brief 调用模型和工具生成回复；群聊可附带 Router 策略

#pragma once

#include <optional>

#include <drogon/utils/coroutine.h>

#include <infrastructure/SessionId.hpp>
#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/runtime/AgentTypes.hpp>
#include <workflow/history/ChatRecordManager.hpp>

namespace insoulforge::ExecutorAgent {
    /// @brief 构建回复 system 提示词，保留全局内容，追加群设定和可用 MCP 服务目录。
    [[nodiscard]] auto buildSystemPrompt(const SessionId &sessionId) -> std::string;
    /// @brief 执行层的回复生成接口
    /// @brief 清理模型输出中的工具调用标签等污染内容
    /// @param text 原始内容
    /// @return 清理后的内容
    [[nodiscard]] auto cleanReplyContent(const std::string &text) -> std::string;

    /// @brief 更新请求副本初始 user JSON 的最后一个字段 execution_state，保留其他上下文和工具历史。
    /// @param currentRound 当前轮次，从 1 开始。
    /// @param maxToolRounds 本次回复固定的总轮数上限。
    /// @param mcpState 当前 MCP 额度及选择数量；无可用 MCP 或最后一轮时省略。
    [[nodiscard]] auto buildIterationMessages(json messages, i32 currentRound, i32 maxToolRounds,
      json mcpState = nullptr) -> json;

    /// @brief 执行回复生成
    /// @param chatRecords 本轮冻结的完整消息快照，同时供模型投影和工具读取；调用方须持有至协程完成。
    /// @param memory 记忆管理器
    /// @param decision 群聊 Router 的决策结果；私聊省略，不注入回复策略。
    /// @param triggerMessageId 用于从冻结快照读取真实工具调用来源；未提供或已不在快照时使用 system。
    /// @return 回复或不回复决策；有工具历史的流程失败附带 failureReason，无可记录内容时返回空值。
    [[nodiscard]] auto execute(const ChatRecordManager &chatRecords, const MemoryManager &memory,
      std::optional<RouterDecision> decision = std::nullopt, std::string triggerMessageId = {})
      -> drogon::Task<std::optional<ReplyDecision>>;
} // namespace insoulforge::ExecutorAgent
