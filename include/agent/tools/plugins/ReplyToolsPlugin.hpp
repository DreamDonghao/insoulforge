/// @file ReplyToolsPlugin.hpp
/// @brief 回复工具插件声明

#pragma once

#include <agent/tools/ToolPlugin.hpp>

namespace insoulforge {
    /// @brief 注册 reply、reply_with_quote、no_reply 等终结本轮的回复工具
    class ReplyToolsPlugin final : public ToolPlugin {
    public:
        /// @brief 获取固定插件 ID builtin.reply
        [[nodiscard]] auto id() const noexcept -> std::string_view override;

        /// @brief 注册回复工具
        /// @param registry 当前插件的注册中心
        void registerTools(ToolRegistry &registry) const override;
    };
} // namespace insoulforge
