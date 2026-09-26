/// @file ExecutorAgent.hpp
/// @brief 根据 Router 策略调用模型和工具，生成本轮回复决策

#pragma once
#include <optional>

#include <drogon/utils/coroutine.h>

#include <agent/memory/MemoryManager.hpp>
#include <agent/runtime/AgentTypes.hpp>
#include <conversation/history/ChatRecordManager.hpp>

namespace insoulforge::ExecutorAgent {
    /// @brief 执行层的回复生成接口
    /// @brief 清理模型输出中的工具调用标签等污染内容
    /// @param text 原始内容
    /// @return 清理后的内容
    [[nodiscard]] auto cleanReplyContent(const std::string &text) -> std::string;

    /// @brief 执行回复生成
    /// @param chatRecords 聊天记录
    /// @param memory 记忆管理器
    /// @param decision Router 的决策结果（包含回复策略）
    /// @param messageSnapshot 本轮冻结的完整消息快照，工具可读取未投影的媒体来源
    /// @return 回复或不回复的决策；模型请求失败且无法形成决策时返回空值
    [[nodiscard]] auto execute(const ChatRecordManager &chatRecords, const MemoryManager &memory,
      RouterDecision decision, json messageSnapshot = {}) -> drogon::Task<std::optional<ReplyDecision>>;
} // namespace insoulforge::ExecutorAgent
