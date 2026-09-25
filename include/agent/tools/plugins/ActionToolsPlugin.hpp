/// @file ActionToolsPlugin.hpp
/// @brief 动作工具插件声明

#pragma once

#include <agent/tools/ToolPlugin.hpp>

namespace insoulforge {
    /// @brief 注册表情、群互动、定时任务等产生副作用的动作工具
    class ActionToolsPlugin final : public ToolPlugin {
    public:
        /// @brief 获取固定插件 ID builtin.action
        [[nodiscard]] auto id() const noexcept -> std::string_view override;

        /// @brief 注册动作工具
        /// @param registry 当前插件的注册中心
        void registerTools(ToolRegistry &registry) const override;
    };
} // namespace insoulforge
