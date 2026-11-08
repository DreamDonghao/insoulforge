/// @file AgentTypes.hpp
/// @brief Agent 类型定义 - 两层代理架构的核心数据结构
/// @details 定义两层代理架构中使用的核心数据类型：
///          - RouterDecision: Router Agent 的路由决策（含回复策略）

#pragma once

#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

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

        bool shouldReply = true;
        std::string tone = "friendly";
        std::optional<i32> maxLength = 25; ///< 未指定时由回复模型根据问题与人设决定长度
        bool isPriority = false;
        bool isPrivate = false; ///< Router 决策所属的会话类型；Executor 独立从会话 ID 选择人设

        [[nodiscard]] static constexpr auto actionToString(Action a) -> std::string_view {
            constexpr std::array names = {"skip", "reply"};
            return names[static_cast<size_t>(a)];
        }
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
