/// @file AgentTypes.hpp
/// @brief Agent 类型定义 - 两层代理架构的核心数据结构
/// @details 定义两层代理架构中使用的核心数据类型：
///          - RouterDecision: Router Agent 的路由决策（含回复策略）
///          - ReplyDecision: Executor Agent 的回复结果

#pragma once

#include <fmt/format.h>

#include <array>
#include <format>
#include <string>
#include <string_view>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief Router Agent 决策结果（合并了规划功能）
    struct RouterDecision {
        enum class Action {
            SKIP, ///< 不处理
            REPLY ///< 需要回复
        };

        Action action = Action::SKIP;
        std::string reason;

        // 回复策略
        bool shouldReply = true;
        std::string tone = "friendly";
        i32 maxLength = 25;
        bool isPriority = false;
        bool isPrivate = false; ///< Router 决策所属的会话类型；Executor 独立从会话 ID 选择人设

        [[nodiscard]] static constexpr auto actionToString(Action a) -> std::string_view {
            constexpr std::array names = {"skip", "reply"};
            return names[static_cast<size_t>(a)];
        }
    };

    /// @brief Executor Agent 回复结果
    struct ReplyDecision {
        bool shouldReply = false;
        std::string content;
        json toolHistory = json::array(); ///< 按实际处理顺序记录工具与参数，不保留工具完整输出
        std::string failureReason; ///< 无法完成回复流程时的原因，与主动 no_reply 区分
    };
} // namespace insoulforge

// fmt::formatter 特化
template<>
struct fmt::formatter<insoulforge::RouterDecision::Action> : formatter<string_view> {
    template<typename FormatContext>
    auto format(const insoulforge::RouterDecision::Action a, FormatContext &ctx) const {
        return formatter<string_view>::format(insoulforge::RouterDecision::actionToString(a), ctx);
    }
};

// std::formatter 特化
template<>
// 标准 C++20 定制点：为用户类型特化 std::formatter（cert-dcl58-cpp 误报，显式豁免）
struct std::formatter<insoulforge::RouterDecision::Action>
    : std::formatter<std::string_view> { // NOLINT(cert-dcl58-cpp)
    template<typename FormatContext>
    auto format(const insoulforge::RouterDecision::Action a, FormatContext &ctx) const {
        return std::formatter<std::string_view>::format(insoulforge::RouterDecision::actionToString(a), ctx);
    }
};
