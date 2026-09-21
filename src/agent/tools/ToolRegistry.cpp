/// @file ToolRegistry.cpp
/// @brief 工具注册中心 - 实现
/// @author donghao
/// @date 2026-03-28

#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/logging/Logger.hpp>

using insoulforge::json;

namespace {
    /// @brief 构建单个工具的 OpenAI function calling 定义（缺省字段补齐为合法 schema）
    json buildToolDef(const insoulforge::Tool &tool) {
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
    ToolRegistry &ToolRegistry::instance() {
        static ToolRegistry registry;
        return registry;
    }

    bool ToolRegistry::registerPlugin(std::string pluginId, const PluginRegistrar &registrar) {
        if (pluginId.empty() || !registrar || !m_activePluginId.empty()) {
            Logger::error(0, "Tool", fmt::format("工具插件注册失败：插件 ID、注册函数无效或发生嵌套注册"));
            return false;
        }

        std::vector<RegisteredTool> previousTools;
        if (const auto previous = m_pluginTools.find(pluginId); previous != m_pluginTools.end()) {
            previousTools.reserve(previous->second.size());
            for (const auto &name: previous->second) {
                if (const auto tool = m_tools.find(name); tool != m_tools.end())
                    previousTools.push_back(tool->second);
            }
        }

        unregisterPlugin(pluginId);
        m_activePluginId = std::move(pluginId);
        m_pluginRegistrationFailed = false;
        try {
            registrar(*this);
        } catch (const std::exception &e) {
            Logger::error(0, "Tool", fmt::format("工具插件 '{}' 注册异常: {}", m_activePluginId, e.what()));
            m_pluginRegistrationFailed = true;
        } catch (...) {
            Logger::error(0, "Tool", fmt::format("工具插件 '{}' 注册发生未知异常", m_activePluginId));
            m_pluginRegistrationFailed = true;
        }

        const std::string registeredPluginId = std::move(m_activePluginId);
        if (m_pluginRegistrationFailed) {
            unregisterPlugin(registeredPluginId);
            for (const auto &tool: previousTools) {
                m_tools.insert_or_assign(tool.tool.name, tool);
                m_pluginTools[registeredPluginId].push_back(tool.tool.name);
            }
            Logger::warn(0, "Tool",
              fmt::format("工具插件 '{}' 注册失败，已恢复此前 {} 个工具", registeredPluginId, previousTools.size()));
            m_pluginRegistrationFailed = false;
            return false;
        }

        const auto registered = m_pluginTools.find(registeredPluginId);
        const size_t count = registered == m_pluginTools.end() ? 0 : registered->second.size();
        Logger::info(0, "Tool", fmt::format("工具插件 '{}' 注册完成（{} 个工具）", registeredPluginId, count));
        return true;
    }

    void ToolRegistry::unregisterPlugin(const std::string &pluginId) {
        const auto it = m_pluginTools.find(pluginId);
        if (it == m_pluginTools.end())
            return;
        for (const auto &name: it->second)
            m_tools.erase(name);
        m_pluginTools.erase(it);
        Logger::info(0, "Tool", fmt::format("工具插件已卸载: {}", pluginId));
    }

    bool ToolRegistry::registerTool(const Tool &tool, const ToolCategory category) {
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
        Logger::info(0, "Tool",
          fmt::format("工具注册成功: {} (插件: {}, 分类: {})", tool.name, pluginId, categoryToString(category)));
        return true;
    }

    json ToolRegistry::getTools(const ToolQuery &query) const {
        json tools = json::array();

        std::vector<const RegisteredTool *> visibleTools;
        visibleTools.reserve(m_tools.size());
        for (const auto &[name, registered]: m_tools) {
            if (query.isPrivateSession && registered.tool.scope == ToolScope::GROUP_ONLY)
                continue;
            visibleTools.push_back(&registered);
        }

        std::ranges::sort(visibleTools, [](const RegisteredTool *lhs, const RegisteredTool *rhs) {
            return std::tuple{categoryOrder(lhs->category), lhs->tool.promptOrder, lhs->tool.name} <
                   std::tuple{categoryOrder(rhs->category), rhs->tool.promptOrder, rhs->tool.name};
        });
        for (const auto *registered: visibleTools)
            tools.push_back(buildToolDef(registered->tool));

        return tools;
    }

    json ToolRegistry::getAllTools() const { return getTools({}); }

    drogon::Task<std::string> ToolRegistry::executeTool(const std::string name, json args, ToolCallContext ctx) const {
        if (const auto it = m_tools.find(name); it != m_tools.end()) {
            co_return co_await it->second.tool.handler(std::move(args), std::move(ctx));
        }
        co_return "工具未找到: " + name;
    }

    bool ToolRegistry::hasTool(const std::string &name) const { return m_tools.contains(name); }

    void ToolRegistry::unregisterTool(const std::string &name) {
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
        Logger::info(0, "Tool", fmt::format("工具已注销: {}", name));
    }

    std::string ToolRegistry::categoryToString(const ToolCategory category) {
        switch (category) {
            case ToolCategory::REPLY:
                return "REPLY";
            case ToolCategory::INFORMATION:
                return "INFORMATION";
            case ToolCategory::ACTION:
                return "ACTION";
            default:
                return "UNKNOWN";
        }
    }

    int ToolRegistry::categoryOrder(const ToolCategory category) {
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
