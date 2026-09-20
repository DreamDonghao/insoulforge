/// @file ToolRuntime.cpp
/// @brief 工具运行时的插件装配与自定义工具加载

#include <optional>
#include <tuple>
#include <utility>

#include <agent/tools/ToolPluginCatalog.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/ToolStore.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <spdlog/spdlog.h>

namespace insoulforge {
    namespace {
        /// @brief 解析并规范化自定义工具的参数 Schema
        [[nodiscard]] json parseCustomToolParameters(const ToolStore::CustomTool &tool) {
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
        [[nodiscard]] std::optional<Tool> makeCustomTool(const ToolStore::CustomTool &tool, json parameters) {
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

    void ToolRuntime::registerBuiltinTools() {
        ToolPluginCatalog::registerBuiltinPlugins();
        spdlog::info("ToolRuntime: 内置工具注册完成，当前工具总数 {}", ToolRegistry::instance().getAllTools().size());
    }

    void ToolRuntime::reloadCustomTools() {
        auto &registry = ToolRegistry::instance();
        const auto tools = ToolStore::getEnabledCustomTools();
        int registeredCount = 0;

        // 重载只替换 custom 插件，不会影响内置工具。
        const bool registered =
          registry.registerPlugin("custom", [&tools, &registeredCount](ToolRegistry &pluginRegistry) {
              for (const auto &tool: tools) {
                  auto definition = makeCustomTool(tool, parseCustomToolParameters(tool));
                  if (!definition) {
                      spdlog::warn("ToolRuntime: 跳过不支持的自定义工具 '{}' ({})", tool.name, tool.executorType);
                      continue;
                  }
                  if (pluginRegistry.registerTool(*definition, ToolCategory::INFORMATION)) {
                      ++registeredCount;
                      spdlog::info("ToolRuntime: 注册自定义工具 '{}' ({})", tool.name, tool.executorType);
                  }
              }
          });

        if (!registered) {
            spdlog::error("ToolRuntime: 自定义工具重载失败，已保留此前注册结果");
            return;
        }
        spdlog::info("ToolRuntime: 自定义工具重载完成（共{}个）", registeredCount);
    }
} // namespace insoulforge
