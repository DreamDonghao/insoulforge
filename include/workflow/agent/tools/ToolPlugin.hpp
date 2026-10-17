/// @file ToolPlugin.hpp
/// @brief 内置工具插件的显式接口
/// @details 插件由 ToolPluginCatalog 显式组合，避免宏和静态初始化副作用。

#pragma once

#include <string_view>

namespace insoulforge {
    class ToolRegistry;

    /// @brief 一个可独立注册、重载和卸载的工具能力域
    /// @details 实现类应聚合相关工具，不应承载 Agent 路由或回复决策逻辑。
    class ToolPlugin {
    public:
        virtual ~ToolPlugin() = default;

        /// @brief 返回全局唯一且稳定的插件标识
        /// @return 小写命名空间形式的插件 ID，如 builtin.weather
        [[nodiscard]] virtual auto id() const noexcept -> std::string_view = 0;

        /// @brief 向当前插件注册回调提供的注册中心添加工具
        /// @param registry 当前插件专属的工具注册入口
        virtual void registerTools(ToolRegistry &registry) const = 0;
    };
} // namespace insoulforge
