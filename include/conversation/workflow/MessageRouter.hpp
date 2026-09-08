/// @file MessageRouter.hpp
/// @brief 基于完整消息快照的 Router

#pragma once

#include <agent/runtime/AgentTypes.hpp>
#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MessageRouter {
    /// @brief 判断是否应回复快照中的最新消息并生成回复策略
    /// @param sessionId 所属会话 ID
    /// @param snapshot 按时间顺序排列的完整消息快照
    /// @return Router 决策；快照为空时返回跳过决策
    /// @details Router 的全部 LLM 上下文由快照投影生成，不读取 OneBot 原始上报或旧聊天记录包装。
    [[nodiscard]] drogon::Task<RouterDecision> route(uint64_t sessionId, const json &snapshot);
} // namespace insoulforge::MessageRouter
