/// @file ToolPluginCatalog.cpp
/// @brief 内置工具插件组合根

#include <agent/tools/plugins/ActionToolsPlugin.hpp>
#include <agent/tools/plugins/InfoToolsPlugin.hpp>
#include <agent/tools/plugins/ReplyToolsPlugin.hpp>
#include <include/agent/tools/ToolRegistry.hpp>

namespace insoulforge::ToolPluginCatalog {
    /// @brief 构造并注册全部编译期内置插件
    /// @details 插件对象仅在注册期间存活；ToolRegistry 保存工具定义和处理器副本。
    void registerBuiltinPlugins() {
        // 新增独立工具域时，仅在此处加入插件实例；Agent 主流程无需改动。
        const ReplyToolsPlugin replyTools;
        const InfoToolsPlugin infoTools;
        const ActionToolsPlugin actionTools;
        const std::array<const ToolPlugin *, 3> plugins = {
          &replyTools,
          &infoTools,
          &actionTools,
        };
        auto &registry = ToolRegistry::instance();
        for (const auto *plugin: plugins) {
            registry.registerPlugin(std::string(plugin->id()),
              [plugin](ToolRegistry &pluginRegistry) -> void { plugin->registerTools(pluginRegistry); });
        }
    }
} // namespace insoulforge::ToolPluginCatalog
