/// @file ActivityToolsPlugin.cpp
/// @brief 活动名称与业务参数由模型选择，身份和生命周期由管理器负责。
#include <array>

#include <workflow/activity/ActivityManager.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/plugins/ActivityToolsPlugin.hpp>

namespace insoulforge {
    auto ActivityToolsPlugin::id() const noexcept -> std::string_view { return "builtin.activity"; }

    void ActivityToolsPlugin::registerTools(ToolRegistry &registry) const {
        struct Definition {
            std::string name;
            std::string operation;
            std::string description;
        };
        const auto definitions = std::array{
          Definition{
            "start_activity", "start", "启动一个全局单例活动。所有会话共享，同名活动不能重复启动。可返回参与链接。"},
          Definition{"command_activity", "command",
            "操作或指挥正在运行的全局活动。传入指令及可选参数，暂停、恢复等规则由活动解释。"},
          Definition{"stop_activity", "stop", "请求结束全局活动。请求受理不等于已经结束，需以活动确认的状态为准。"},
          Definition{"get_activity_details", "details", "实时查询活动的完整信息和支持的操作；上下文中只有简要状态。"},
        };
        for (const auto &definition: definitions) {
            auto properties = json{{"name", {{"type", "string"}, {"description", "可用活动目录中的名称"}}}};
            auto required = json::array({"name"});
            if (definition.operation == "start" || definition.operation == "command")
                properties["parameters"] = {{"type", "object"}, {"description", "交给活动解释的可选 JSON 参数"}};
            if (definition.operation == "command") {
                properties["instruction"] = {{"type", "string"}, {"description", "希望活动执行的指令"}};
                required.push_back("instruction");
            }
            registry.registerTool(
              {
                .name = definition.name,
                .description = definition.description,
                .parameters = {{"type", "object"}, {"properties", properties}, {"required", required},
                  {"additionalProperties", false}},
                .handler = [operation = definition.operation](
                             json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                    const auto name = getStr(args, "name");
                    if (name.empty())
                        co_return "需要活动名称";
                    if (args.contains("parameters") && !args["parameters"].is_object())
                        co_return "parameters 必须是 JSON 对象";
                    if (operation == "command" && getStr(args, "instruction").empty())
                        co_return "需要非空操作指令";
                    auto arguments = json{{"parameters", args.value("parameters", json::object())}};
                    if (operation == "command")
                        arguments["instruction"] = getStr(args, "instruction");
                    auto origin =
                      json{{"session_id", ctx.requireSession().toString()}, {"user_id", ctx.callerUserId.toString()}};
                    const auto result = co_await ActivityManager::instance().invoke(
                      operation, name, std::move(arguments), std::move(origin));
                    co_return dumpJson(result);
                },
              },
              definition.operation == "details" ? ToolCategory::INFORMATION : ToolCategory::ACTION);
        }
    }
} // namespace insoulforge
