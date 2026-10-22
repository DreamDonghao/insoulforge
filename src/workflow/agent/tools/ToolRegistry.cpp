/// @file ToolRegistry.cpp
/// @brief 工具注册中心 - 实现

#include <mutex>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>

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
        for (const auto &name: staged.m_tools | std::views::keys) {
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
        if (auto &names = m_pluginTools[pluginId]; !std::ranges::contains(names, tool.name))
            names.push_back(tool.name);
        return true;
    }

    auto ToolRegistry::getTools(const ToolQuery &query) const -> json {
        std::shared_lock lock(m_mutex);
        json tools = json::array();

        std::vector<const RegisteredTool *> visibleTools;
        visibleTools.reserve(m_tools.size());
        for (const auto &registered: m_tools | std::views::values) {
            if (!isEnabled(registered))
                continue;
            if (!query.includeAllMcp) {
                if (registered.tool.mcpManagement && !query.mcpManagement)
                    continue;
                if (registered.tool.mcpVersion != 0) {
                    const auto selected = query.mcpTools.find(registered.tool.name);
                    if (selected == query.mcpTools.end() || selected->second != registered.tool.mcpVersion)
                        continue;
                }
            }
            if (query.isPrivateSession && registered.tool.scope == ToolScope::GROUP_ONLY)
                continue;
            if (query.replyOnly && registered.category != ToolCategory::REPLY)
                continue;
            visibleTools.push_back(&registered);
        }

        std::ranges::sort(visibleTools, [](const RegisteredTool *lhs, const RegisteredTool *rhs) -> bool {
            return std::tuple{categoryOrder(lhs->category), lhs->tool.promptOrder, lhs->tool.name} <
                   std::tuple{categoryOrder(rhs->category), rhs->tool.promptOrder, rhs->tool.name};
        });
        for (const auto *registered: visibleTools) {
            auto definition = buildToolDef(registered->tool);
            if (registered->pluginId.starts_with("builtin.")) {
                const auto &override =
                  atOrNull(atOrNull(m_builtinConfig, registered->tool.name.c_str()), "description");
                if (override.is_string())
                    definition["function"]["description"] = override;
            }
            tools.push_back(std::move(definition));
        }

        return tools;
    }

    auto ToolRegistry::getAllTools() const -> json { return getTools({.includeAllMcp = true}); }

    auto ToolRegistry::executeTool(const std::string name, json args, ToolCallContext ctx) const
      -> drogon::Task<std::string> {
        ToolHandler handler;
        {
            std::shared_lock lock(m_mutex);
            if (const auto it = m_tools.find(name); it != m_tools.end()) {
                if (!isEnabled(it->second))
                    co_return "工具已关闭，未执行: " + name;
                if (ctx.replyOnly && it->second.category != ToolCategory::REPLY) {
                    Logger::warn(ctx.sessionId, "Tool", fmt::format("最后一轮拒绝非回复工具: {}", name));
                    co_return "最后一轮只允许回复类工具，未执行: " + name;
                }
                const auto offered = ctx.offeredMcpTools.find(name);
                if (it->second.tool.mcpVersion != 0 || offered != ctx.offeredMcpTools.end()) {
                    if (offered == ctx.offeredMcpTools.end() || offered->second != it->second.tool.mcpVersion)
                        co_return "MCP 工具未在本轮开放或定义已更新，请查询并启用后在下一轮调用：" + name;
                }
                if (it->second.tool.mcpManagement && !ctx.mcpSelection)
                    co_return "MCP 管理工具缺少当前回复的选择状态";
                handler = it->second.tool.handler;
            }
        }
        if (handler) {
            try {
                co_return co_await handler(std::move(args), std::move(ctx));
            } catch (const std::exception &error) {
                Logger::error(ctx.sessionId, "Tool", fmt::format("工具 '{}' 执行异常: {}", name, error.what()));
                co_return fmt::format("工具 '{}' 执行失败: {}", name, std::string_view(error.what()).substr(0, 500));
            } catch (...) {
                Logger::error(ctx.sessionId, "Tool", fmt::format("工具 '{}' 执行发生未知异常", name));
                co_return fmt::format("工具 '{}' 执行失败: 未知异常", name);
            }
        }
        co_return "工具未找到: " + name;
    }

    auto ToolRegistry::hasTool(const std::string &name) const -> bool {
        std::shared_lock lock(m_mutex);
        return m_tools.contains(name);
    }

    auto ToolRegistry::isEnabled(const RegisteredTool &registered) const -> bool {
        return !registered.pluginId.starts_with("builtin.") ||
               getBool(atOrNull(m_builtinConfig, registered.tool.name.c_str()), "enabled", true);
    }

    auto ToolRegistry::isToolDisabled(const std::string &name) const -> bool {
        std::shared_lock lock(m_mutex);
        const auto it = m_tools.find(name);
        return it != m_tools.end() && !isEnabled(it->second);
    }

    void ToolRegistry::loadBuiltinToolsConfig(json config) {
        std::unique_lock lock(m_mutex);
        m_builtinConfig = config.is_object() ? std::move(config) : json::object();
    }

    auto ToolRegistry::getBuiltinTools() const -> json {
        std::shared_lock lock(m_mutex);
        json result = json::array();
        for (const auto &[name, registered]: m_tools) {
            if (!registered.pluginId.starts_with("builtin."))
                continue;
            const auto &description = atOrNull(atOrNull(m_builtinConfig, name.c_str()), "description");
            const auto category = registered.category == ToolCategory::REPLY         ? "回复"
                                  : registered.category == ToolCategory::INFORMATION ? "信息"
                                                                                     : "动作";
            result.push_back({{"name", name}, {"category", category}, {"enabled", isEnabled(registered)},
              {"description", description.is_string() ? description : json(registered.tool.description)},
              {"defaultDescription", registered.tool.description}, {"customDescription", description.is_string()}});
        }
        return result;
    }

    auto ToolRegistry::saveBuiltinToolConfig(const std::string &name, const json &patch)
      -> std::expected<void, std::string> {
        if (!patch.is_object() || patch.empty())
            return std::unexpected("请提供 enabled 或 description 配置");
        for (const auto &[key, value]: patch.items()) {
            if (key != "enabled" && key != "description")
                return std::unexpected("不支持的工具配置字段: " + key);
        }
        std::unique_lock lock(m_mutex);
        const auto it = m_tools.find(name);
        if (it == m_tools.end() || !it->second.pluginId.starts_with("builtin."))
            return std::unexpected("内置工具不存在: " + name);
        auto next = m_builtinConfig;
        if (!next.contains(name))
            next[name] = {{"enabled", true}, {"description", nullptr}};
        next[name].update(patch);
        if (const auto saved = Config::instance().saveBuiltinToolsConfig(next); !saved)
            return std::unexpected(saved.error().message);
        m_builtinConfig = std::move(next);
        return {};
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
