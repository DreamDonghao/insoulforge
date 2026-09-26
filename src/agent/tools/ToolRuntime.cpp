/// @file ToolRuntime.cpp
/// @brief 工具运行时的插件装配与自定义工具加载


#include <agent/tools/ToolPluginCatalog.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/custom/ToolStore.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        /// @brief 解析并规范化自定义工具的参数 Schema
        [[nodiscard]] auto parseCustomToolParameters(const ToolStore::CustomTool &tool) -> json {
            json parameters;
            if (!tool.parameters.empty()) {
                std::ignore = tryParseJson(tool.parameters, parameters);
            }
            if (!parameters.is_null() && !parameters.contains("type")) {
                parameters["type"] = "object";
            }
            return parameters;
        }

        /// @brief 按执行器类型构建自定义工具定义
        [[nodiscard]] auto makeCustomTool(const ToolStore::CustomTool &tool, json parameters) -> std::optional<Tool> {
            if (tool.executorType == "python") {
                return Tool{
                  .name = tool.name,
                  .description = tool.description,
                  .parameters = std::move(parameters),
                  .handler = [script = tool.scriptContent](json args, ToolCallContext) -> drogon::Task<std::string> {
                      co_return co_await ToolRuntime::executePythonTool(script, std::move(args));
                  },
                };
            }
            if (tool.executorType == "http") {
                return Tool{
                  .name = tool.name,
                  .description = tool.description,
                  .parameters = std::move(parameters),
                  .handler = [config = tool.executorConfig](
                               json args, ToolCallContext context) -> drogon::Task<std::string> {
                      co_return co_await ToolRuntime::executeHttpTool(config, std::move(args), context.sessionId);
                  },
                };
            }
            return std::nullopt;
        }
    } // namespace

    void ToolRuntime::registerBuiltinTools() { ToolPluginCatalog::registerBuiltinPlugins(); }

    void ToolRuntime::reloadCustomTools() {
        auto &registry = ToolRegistry::instance();
        const auto tools = ToolStore::getEnabledCustomTools();
        i32 registeredCount = 0;

        // 重载只替换 custom 插件，不会影响内置工具。
        const bool registered =
          registry.registerPlugin("custom", [&tools, &registeredCount](ToolRegistry &pluginRegistry) -> void {
              for (const auto &tool: tools) {
                  auto definition = makeCustomTool(tool, parseCustomToolParameters(tool));
                  if (!definition) {
                      Logger::warn(0, "Tool",
                        fmt::format("ToolRuntime: 跳过不支持的自定义工具 '{}' ({})", tool.name, tool.executorType));
                      continue;
                  }
                  if (pluginRegistry.registerTool(*definition, ToolCategory::INFORMATION)) {
                      ++registeredCount;
                      Logger::info(
                        0, "Tool", fmt::format("ToolRuntime: 注册自定义工具 '{}' ({})", tool.name, tool.executorType));
                  }
              }
          });

        if (!registered) {
            Logger::error(0, "Tool", fmt::format("ToolRuntime: 自定义工具重载失败，已保留此前注册结果"));
            return;
        }
        Logger::info(0, "Tool", fmt::format("ToolRuntime: 自定义工具重载完成（共{}个）", registeredCount));
    }
} // namespace insoulforge
