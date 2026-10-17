/// @file ToolPluginCatalog.hpp
/// @brief 内置工具插件的组合根
/// @details 仅维护插件实例列表，ToolRuntime 不直接依赖具体插件。

#pragma once

#include <string>
#include <string_view>

#include <expected>

namespace insoulforge::ToolPluginCatalog {
    /// @brief 显式加载全部内置插件
    /// @details 新增插件仅需在 Catalog 实例列表增加该插件，不修改 Agent 主流程。
    [[nodiscard]] auto registerBuiltinPlugins(std::string_view botName) -> std::expected<void, std::string>;
} // namespace insoulforge::ToolPluginCatalog
