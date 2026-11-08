/// @file ActivityToolsPlugin.cpp
/// @brief 转发 Activity 自定义参数，不解释业务或注入调用身份。
#include <workflow/activity/ActivityManager.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/plugins/ActivityToolsPlugin.hpp>

namespace insoulforge {
    auto ActivityToolsPlugin::id() const noexcept -> std::string_view { return "builtin.activity"; }
    void ActivityToolsPlugin::registerTools(ToolRegistry &registry) const {
        registry.registerTool(
          {
            .name = "request_activity",
            .description =
              "向可用 Activity 发送请求。parameters 按该服务的 instructions 构造；查询与操作均使用此工具。附件返回 "
              "image_id，可供图片工具使用。超时或断线不代表业务未执行，不要自动重试。",
            .parameters = {{"type", "object"},
              {"properties", {{"name", {{"type", "string"}, {"description", "可用 Activity 名称"}}},
                               {"parameters", {{"type", "object"}, {"description", "由 Activity 约定的请求参数"}}}}},
              {"required", {"name", "parameters"}}, {"additionalProperties", false}},
            .handler = [](json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                if (getStr(args, "name").empty() || !atOrNull(args, "parameters").is_object()) {
                    co_return "需要 name 和 parameters 对象";
                }
                auto response = co_await ActivityManager::instance().invoke(getStr(args, "name"), args["parameters"]);
                if (ctx.toolImages && atOrNull(response, "attachments").is_array()) {
                    for (const auto &attachment: response["attachments"]) {
                        if (auto image = Image::find(getStr(attachment, "image_id")); image) {
                            ctx.toolImages->push_back(std::move(*image));
                        }
                    }
                }
                co_return dumpJson(response);
            },
          },
          ToolCategory::ACTION);
    }
} // namespace insoulforge
