/// @file MessageRouter.hpp
/// @brief 基于完整消息快照的 Router

#pragma once

#include <optional>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <workflow/agent/runtime/AgentTypes.hpp>

namespace insoulforge::MessageRouter {
    /// @brief 结合历史、新入站消息及末尾已发送内容，判断当前是否还需回复。
    /// @param sessionId 所属会话 ID
    /// @param snapshot 本次判断的冻结消息窗口；调用方须持有至协程完成。
    /// @return 回复策略；空快照跳过，群自动回复关闭时仅允许必须回复的消息。
    /// @note 不修改消息处理标记；仅有 silent 或没有未处理入站消息时跳过；其他消息先应用硬规则，再由 Jev、LLM
    /// 判断上下文。
    [[nodiscard]] auto route(SessionId sessionId, const json &snapshot) -> drogon::Task<RouterDecision>;

    /// @brief 把 Jev action choice 的胜出 label 映射为路由动作
    /// @param label Jev choice 答案的胜出 label（skip / reply / unclear）
    /// @param confidence Jev 返回的有效置信度；缺失时不采纳 Jev 决策
    /// @param minConfidence 配置的最低置信度
    /// @return 达到阈值的 skip/reply 返回对应动作；否则交给 LLM 判断
    [[nodiscard]] auto classifyJevChoice(std::string_view label, std::optional<double> confidence, f64 minConfidence)
      -> std::optional<RouterDecision::Action>;

    /// @brief 计算 Router 上下文窗口的起始下标
    /// @param snapshotSize 完整快照的消息条数
    /// @details 该窗口按消息条数分段扩展：窗口大小在 [keep, keep + slide - 1] 之间周期性
    ///          变化（默认 10~19），同一批次内起始下标保持不变，只有跨越批次边界时前缀
    ///          才整体前移。此设计意在保留稳定的上下文前缀，提高请求缓存命中率；逐条
    ///          滑动会让聊天记录部分几乎每轮都变，命中率显著下降。
    ///          仅 Router LLM 使用此窗口；Jev 独立读取最近 jevWindowSize 条消息。
    ///          导出到头文件用于支持无网络的离线测试，与请求构建共用同一窗口算法。
    [[nodiscard]] auto windowStartIndex(size_t snapshotSize) -> size_t;
} // namespace insoulforge::MessageRouter
