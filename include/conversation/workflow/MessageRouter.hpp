/// @file MessageRouter.hpp
/// @brief 基于完整消息快照的 Router

#pragma once

#include <agent/runtime/AgentTypes.hpp>
#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MessageRouter {
    /// @brief 判断是否应回复指定触发消息并生成回复策略
    /// @param sessionId 所属会话 ID
    /// @param triggerMessageId 本轮触发回复判断的入站消息 ID
    /// @param snapshot 按时间顺序排列的完整消息快照
    /// @return Router 决策；找不到触发消息或快照为空时返回跳过决策
    /// @details 触发消息决定硬规则，完整快照仅提供上下文，允许其末条为机器人已经发送的消息。
    [[nodiscard]] drogon::Task<RouterDecision> route(
      uint64_t sessionId, std::string_view triggerMessageId, const json &snapshot);
} // namespace insoulforge::MessageRouter
