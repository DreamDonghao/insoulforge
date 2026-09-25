/// @file InfoToolsPlugin.hpp
/// @brief 信息工具插件声明

#pragma once

#include <agent/tools/ToolPlugin.hpp>

namespace insoulforge {
    /// @brief 注册记忆、表情列表、深度思考等无副作用的信息工具
    class InfoToolsPlugin final : public ToolPlugin {
    public:
        /// @brief 获取固定插件 ID builtin.info
        [[nodiscard]] auto id() const noexcept -> std::string_view override;

        /// @brief 注册信息工具
        /// @param registry 当前插件的注册中心
        void registerTools(ToolRegistry &registry) const override;
    };
} // namespace insoulforge
