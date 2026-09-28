/// @file MessageRouter.hpp
/// @brief 基于完整消息快照的 Router

#pragma once

#include <drogon/utils/coroutine.h>

#include <agent/runtime/AgentTypes.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::MessageRouter {
    /// @brief 判断是否应回复指定触发消息并生成回复策略
    /// @param sessionId 所属会话 ID
    /// @param triggerMessageId 本轮触发回复判断的入站消息 ID
    /// @param snapshot 按时间顺序排列的完整消息快照
    /// @return Router 决策；找不到触发消息或快照为空时返回跳过决策
    /// @details 触发消息决定硬规则，完整快照仅提供上下文，允许其末条为机器人已经发送的消息。
    [[nodiscard]] auto route(u64 sessionId, std::string_view triggerMessageId, const json &snapshot)
      -> drogon::Task<RouterDecision>;

    /// @brief 计算 Router 上下文窗口的起始下标
    /// @param snapshotSize 完整快照的消息条数
    /// @details 该窗口按消息条数分段扩展：窗口大小在 [keep, keep + slide - 1] 之间周期性
    ///          变化（默认 10~19），同一批次内起始下标保持不变，只有跨越批次边界时前缀
    ///          才整体前移。此设计意在保留稳定的上下文前缀，提高请求缓存命中率；逐条
    ///          滑动会让聊天记录部分几乎每轮都变，命中率显著下降。
    ///          优先路径（Jev）与兜底路径（LLM）共用这一语义，均通过本函数取窗口起点。
    ///          导出到头文件用于支持无网络的离线测试，保持 route() 的既有签名不变。
    [[nodiscard]] auto windowStartIndex(const size_t snapshotSize) -> size_t;
} // namespace insoulforge::MessageRouter
