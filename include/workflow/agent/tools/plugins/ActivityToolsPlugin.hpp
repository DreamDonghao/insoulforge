/// @file ActivityToolsPlugin.hpp
/// @brief 独立 Activity 服务的通用模型工具。
#pragma once
#include <workflow/agent/tools/ToolPlugin.hpp>

namespace insoulforge {
    class ActivityToolsPlugin final : public ToolPlugin {
    public:
        [[nodiscard]] auto id() const noexcept -> std::string_view override;
        void registerTools(ToolRegistry &registry) const override;
    };
} // namespace insoulforge
