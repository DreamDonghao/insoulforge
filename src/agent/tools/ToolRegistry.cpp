/// @file ToolRegistry.cpp
/// @brief 工具注册中心 - 实现

#include <mutex>

#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>

using insoulforge::json;

namespace {
    /// @brief 构建单个工具的 OpenAI function calling 定义（缺省字段补齐为合法 schema）
    auto buildToolDef(const insoulforge::Tool &tool) -> json {
        json toolDef;
        toolDef["type"] = "function";
        toolDef["function"]["name"] = tool.name;
        toolDef["function"]["description"] = tool.description;
        json params = tool.parameters.is_null() ? json::object() : tool.parameters;
        params["type"] = "object";
        if (!params.contains("properties")) {
            params["properties"] = json::object();
        }
        if (!params.contains("required")) {
            params["required"] = json::array();
        }
        toolDef["function"]["parameters"] = params;
        return toolDef;
    }
} // namespace

namespace insoulforge {
    auto ToolRegistry::instance() -> ToolRegistry & {
        static ToolRegistry registry;
        return registry;
    }

    auto ToolRegistry::registerPlugin(std::string pluginId, const PluginRegistrar &registrar) -> bool {
        if (pluginId.empty() || !registrar || !m_activePluginId.empty()) {
            Logger::error(0, "Tool", fmt::format("工具插件注册失败：插件 ID、注册函数无效或发生嵌套注册"));
            return false;
        }
        ToolRegistry staged;
        staged.m_activePluginId = pluginId;
        try {
            registrar(staged);
        } catch (const std::exception &e) {
            Logger::error(0, "Tool", fmt::format("工具插件 '{}' 注册异常: {}", pluginId, e.what()));
            return false;
        } catch (...) {
            Logger::error(0, "Tool", fmt::format("工具插件 '{}' 注册发生未知异常", pluginId));
            return false;
        }
        if (staged.m_pluginRegistrationFailed) {
            return false;
        }
        std::unique_lock lock(m_mutex);
        for (const auto &[name, tool]: staged.m_tools) {
            if (const auto existing = m_tools.find(name);
              existing != m_tools.end() && existing->second.pluginId != pluginId) {
                Logger::error(0, "Tool",
                  fmt::format("工具插件 '{}' 与 '{}' 的工具 '{}' 冲突", pluginId, existing->second.pluginId, name));
                return false;
            }
        }
        if (const auto old = m_pluginTools.find(pluginId); old != m_pluginTools.end()) {
            for (const auto &name: old->second) {
                m_tools.erase(name);
            }
            m_pluginTools.erase(old);
        }
        for (auto &[name, tool]: staged.m_tools) {
            m_tools.emplace(name, std::move(tool));
            m_pluginTools[pluginId].push_back(name);
        }
        return true;
    }

    void ToolRegistry::unregisterPlugin(const std::string &pluginId) {
        std::unique_lock lock(m_mutex);
        const auto it = m_pluginTools.find(pluginId);
        if (it == m_pluginTools.end())
            return;
        for (const auto &name: it->second)
            m_tools.erase(name);
        m_pluginTools.erase(it);
    }

    auto ToolRegistry::registerTool(const Tool &tool, const ToolCategory category) -> bool {
        std::unique_lock lock(m_mutex);
        if (tool.name.empty() || !tool.handler) {
            Logger::error(0, "Tool", fmt::format("工具注册失败：工具名称或处理器为空"));
            if (!m_activePluginId.empty())
                m_pluginRegistrationFailed = true;
            return false;
        }

        const std::string pluginId = m_activePluginId.empty() ? "application" : m_activePluginId;
        if (const auto it = m_tools.find(tool.name); it != m_tools.end() && it->second.pluginId != pluginId) {
            Logger::error(0, "Tool",
              fmt::format(
                "工具注册冲突: '{}' 已由插件 '{}' 注册，插件 '{}' 不能覆盖", tool.name, it->second.pluginId, pluginId));
            if (!m_activePluginId.empty())
                m_pluginRegistrationFailed = true;
            return false;
        }

        m_tools.insert_or_assign(tool.name, RegisteredTool{.tool = tool, .category = category, .pluginId = pluginId});
        auto &names = m_pluginTools[pluginId];
        if (!std::ranges::contains(names, tool.name))
            names.push_back(tool.name);
        return true;
    }

    auto ToolRegistry::getTools(const ToolQuery &query) const -> json {
        std::shared_lock lock(m_mutex);
        json tools = json::array();

        std::vector<const RegisteredTool *> visibleTools;
        visibleTools.reserve(m_tools.size());
        for (const auto &[name, registered]: m_tools) {
            if (query.isPrivateSession && registered.tool.scope == ToolScope::GROUP_ONLY)
                continue;
            visibleTools.push_back(&registered);
        }

        std::ranges::sort(visibleTools, [](const RegisteredTool *lhs, const RegisteredTool *rhs) -> bool {
            return std::tuple{categoryOrder(lhs->category), lhs->tool.promptOrder, lhs->tool.name} <
                   std::tuple{categoryOrder(rhs->category), rhs->tool.promptOrder, rhs->tool.name};
        });
        for (const auto *registered: visibleTools)
            tools.push_back(buildToolDef(registered->tool));

        return tools;
    }

    auto ToolRegistry::getAllTools() const -> json { return getTools({}); }

    auto ToolRegistry::executeTool(const std::string name, json args, ToolCallContext ctx) const
      -> drogon::Task<std::string> {
        ToolHandler handler;
        {
            std::shared_lock lock(m_mutex);
            if (const auto it = m_tools.find(name); it != m_tools.end()) {
                handler = it->second.tool.handler;
            }
        }
        if (handler) {
            co_return co_await handler(std::move(args), std::move(ctx));
        }
        co_return "工具未找到: " + name;
    }

    auto ToolRegistry::hasTool(const std::string &name) const -> bool {
        std::shared_lock lock(m_mutex);
        return m_tools.contains(name);
    }

    void ToolRegistry::unregisterTool(const std::string &name) {
        std::unique_lock lock(m_mutex);
        m_tools.erase(name);
        for (auto iterator = m_pluginTools.begin(); iterator != m_pluginTools.end();) {
            auto &names = iterator->second;
            std::erase(names, name);
            if (names.empty()) {
                iterator = m_pluginTools.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    auto ToolRegistry::categoryOrder(const ToolCategory category) -> i32 {
        switch (category) {
            case ToolCategory::REPLY:
                return 0;
            case ToolCategory::INFORMATION:
                return 1;
            case ToolCategory::ACTION:
                return 2;
        }
        return 3;
    }
} // namespace insoulforge
