/// @file AdminControllerSettingsAndTools.cpp
/// @brief 管理后台 REST API 控制器 - 模型、执行流程、提示词和自定义工具设置

#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/mcp/McpTools.hpp>
#include <agent/tools/custom/LuaToolExecutor.hpp>
#include <agent/tools/custom/ToolStore.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <llm/prompts/PromptStore.hpp>

using namespace insoulforge;
using namespace drogon;

// ==================== LLM配置 ====================

auto AdminController::getLLMConfigs(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto configs = Config::instance().getAllLLMConfigs();
    callback(jsonResponse(configs));
    co_return;
}

auto AdminController::saveLLMConfig(
  const HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object() || !body->contains("name")) {
        callback(AdminResponse::configErrorResponse(
          {.type = ConfigErrorType::InvalidArgument, .message = "模型配置必须是包含 name 字段的 JSON 对象"}));
        co_return;
    }

    const std::string name = getStr(*body, "name");
    if (auto result = Config::instance().saveLLMConfig(name, *body); !result) {
        Logger::error(0, "Config", result.error().message);
        callback(AdminResponse::configErrorResponse(result.error()));
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("LLM配置已保存")));
    co_return;
}

// ==================== 执行配置 ====================

auto AdminController::getExecutionConfig(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    callback(jsonResponse(Config::instance().getExecutionConfig()));
    co_return;
}

auto AdminController::saveExecutionConfig(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object()) {
        auto response = jsonResponse(AdminResponse::failJson("执行配置必须是 JSON 对象"));
        response->setStatusCode(k400BadRequest);
        callback(response);
        co_return;
    }

    if (auto result = Config::instance().saveExecutionConfig(*body); !result) {
        Logger::error(0, "Config", result.error().message);
        callback(AdminResponse::configErrorResponse(result.error()));
        co_return;
    }
    callback(jsonResponse(AdminResponse::okJson("执行配置已保存")));
    co_return;
}

// ==================== 提示词 ====================

auto AdminController::getMcpConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto result = Config::instance().getMcpConfig();
    result["status"] = McpTools::instance().status();
    auto response = jsonResponse(result);
    response->addHeader("Cache-Control", "no-store");
    callback(response);
    co_return;
}

auto AdminController::saveMcpConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object()) {
        callback(AdminResponse::configErrorResponse({.type = ConfigErrorType::InvalidArgument, .message = "MCP 配置必须是 JSON 对象"}));
        co_return;
    }
    if (auto result = McpTools::instance().saveConfig(*body, Config::instance()); !result) {
        callback(AdminResponse::configErrorResponse(result.error()));
        co_return;
    }
    // 配置已持久化并生效，网络发现不占用保存请求的等待时间。
    drogon::async_run([]() -> Task<> { co_await McpTools::instance().ensureTools(); });
    callback(jsonResponse(AdminResponse::okJson("MCP 配置已保存并生效")));
    co_return;
}

auto AdminController::refreshMcpTools(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    co_await McpTools::instance().refreshTools();
    auto result = AdminResponse::okJson("MCP 工具刷新完成");
    result["status"] = McpTools::instance().status();
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::getPrompts(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto prompts = PromptStore::getAllPrompts();

    json result;
    for (const auto &[key, content]: prompts) {
        result[key] = content;
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::savePrompt(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("key") || !body->contains("content")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少key或content字段")));
        co_return;
    }

    std::string key = getStr(*body, "key");
    std::string content = getStr(*body, "content");
    std::string description = getStr(*body, "description");

    // 防护: router 提示词的 JSON 格式示例若含双花括号(fmt 转义残留/旧页面缓存内容),模型会照抄导致解析失败
    if ((key == "router_system" || key == "router_private_system") &&
        (content.find("{{") != std::string::npos || content.find("}}") != std::string::npos)) {
        callback(jsonResponse(
          AdminResponse::failJson("提示词包含双花括号{{ }}，JSON 格式示例应为单花括号，请刷新页面后重试")));
        co_return;
    }

    PromptStore::setPrompt(key, content, description);
    Logger::warn(0, "Admin", fmt::format("管理后台更新提示词: key={}, 长度={}", key, content.size()));

    callback(jsonResponse(AdminResponse::okJson("提示词已保存")));
    co_return;
}

// ==================== 自定义工具 ====================

auto AdminController::getCustomTools(HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto tools = ToolStore::getCustomTools();

    json result(json::array());
    for (const auto &[id, name, description, parameters, executorType, scriptContent, readme, enabled]: tools) {
        json item;
        item["id"] = id;
        item["name"] = name;
        item["description"] = description;
        item["parameters"] = parameters;
        item["executorType"] = executorType;
        item["scriptContent"] = scriptContent;
        item["readme"] = readme;
        item["enabled"] = enabled;
        result.push_back(item);
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::addCustomTool(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("name") || !body->contains("executorType")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少必要字段 (name, executorType)")));
        co_return;
    }

    const std::string name = getStr(*body, "name");

    // 检查是否与内置工具名冲突
    if (const auto &registry = ToolRegistry::instance(); registry.hasTool(name)) {
        callback(jsonResponse(AdminResponse::errorJson("工具名 '" + name + "' 已存在（内置工具或自定义工具）")));
        co_return;
    }

    ToolStore::CustomTool tool;
    tool.name = name;
    tool.description = getStr(*body, "description");
    tool.parameters = getStr(*body, "parameters");
    tool.executorType = getStr(*body, "executorType");
    tool.scriptContent = getStr(*body, "scriptContent");
    tool.readme = getStr(*body, "readme");
    tool.enabled = getBool(*body, "enabled", true);

    if (tool.executorType == "lua") {
        if (const auto error = LuaToolExecutor::validate(tool.scriptContent)) {
            callback(jsonResponse(AdminResponse::errorJson(*error)));
            co_return;
        }
    } else {
        callback(jsonResponse(AdminResponse::errorJson("未知的执行类型")));
        co_return;
    }

    i32 id = 0;
    try {
        id = ToolStore::addCustomTool(tool);
    } catch (const std::exception &error) {
        callback(jsonResponse(AdminResponse::failJson(error.what())));
        co_return;
    }

    // 立即注册到 ToolRegistry
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    json resp = AdminResponse::okJson("自定义工具已添加");
    resp["id"] = id;
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::updateCustomTool(const HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("name") || !body->contains("executorType")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少必要字段 (name, executorType)")));
        co_return;
    }

    ToolStore::CustomTool tool;
    tool.id = std::stoi(id);
    tool.name = getStr(*body, "name");
    tool.description = getStr(*body, "description");
    tool.parameters = getStr(*body, "parameters");
    tool.executorType = getStr(*body, "executorType");
    tool.scriptContent = getStr(*body, "scriptContent");
    tool.readme = getStr(*body, "readme");
    tool.enabled = getBool(*body, "enabled", true);

    if (tool.executorType == "lua") {
        if (const auto error = LuaToolExecutor::validate(tool.scriptContent)) {
            callback(jsonResponse(AdminResponse::errorJson(*error)));
            co_return;
        }
    } else {
        callback(jsonResponse(AdminResponse::errorJson("未知的执行类型")));
        co_return;
    }

    try {
        ToolStore::updateCustomTool(tool);
    } catch (const std::exception &error) {
        callback(jsonResponse(AdminResponse::failJson(error.what())));
        co_return;
    }

    // 重新注册工具
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("自定义工具已更新")));
    co_return;
}

auto AdminController::deleteCustomTool(
  HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const i32 toolId = std::stoi(id);
    try {
        ToolStore::deleteCustomTool(toolId);
    } catch (const std::exception &error) {
        callback(jsonResponse(AdminResponse::failJson(error.what())));
        co_return;
    }

    // 重新注册工具（移除已删除的）
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("自定义工具已删除")));
    co_return;
}

auto AdminController::toggleCustomTool(
  HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const i32 toolId = std::stoi(id);
    try {
        ToolStore::toggleCustomTool(toolId);
    } catch (const std::exception &error) {
        callback(jsonResponse(AdminResponse::failJson(error.what())));
        co_return;
    }

    // 重新注册工具
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("工具状态已切换")));
    co_return;
}

auto AdminController::reloadCustomTools(HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("自定义工具已重新加载")));
    co_return;
}

auto AdminController::testCustomTool(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto body = parseJsonBody(req);
    if (!body) {
        callback(jsonResponse(AdminResponse::failJson("缺少请求数据")));
        co_return;
    }

    // 支持两种方式：
    // 1. 传入 toolId 测试已保存的工具
    // 2. 传入工具定义直接测试（未保存）
    std::string executorType;
    std::string scriptContent;
    json testArgs;

    if (body->contains("toolId")) {
        // 从文件目录加载工具
        const i32 toolId = getInt(*body, "toolId");
        auto tools = ToolStore::getCustomTools();
        auto it = std::ranges::find_if(tools, [toolId](const auto &t) -> auto { return t.id == toolId; });
        if (it == tools.end()) {
            callback(jsonResponse(AdminResponse::failJson("工具不存在")));
            co_return;
        }
        executorType = it->executorType;
        scriptContent = it->scriptContent;
        testArgs = body->contains("args") ? (*body)["args"] : json();
    } else {
        // 直接使用传入的定义
        executorType = getStr(*body, "executorType");
        scriptContent = getStr(*body, "scriptContent");
        testArgs = body->contains("args") ? (*body)["args"] : json();
    }

    std::string result;
    if (executorType == "lua") {
        try {
            result = co_await LuaToolExecutor::execute(std::move(scriptContent), std::move(testArgs), 0, false, true);
        } catch (const std::exception &error) {
            callback(jsonResponse(AdminResponse::errorJson(error.what())));
            co_return;
        }
    } else {
        callback(jsonResponse(AdminResponse::failJson("不支持的执行类型，仅支持 Lua")));
        co_return;
    }

    json resp = AdminResponse::okJson();
    resp["result"] = result;
    callback(jsonResponse(resp));
    co_return;
}

// ============== 自定义工具导入导出 ==============

auto AdminController::exportCustomTool(
  HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    i32 toolId = std::stoi(id);
    auto tools = ToolStore::getCustomTools();

    auto it = std::ranges::find_if(tools, [toolId](const ToolStore::CustomTool &t) -> bool { return t.id == toolId; });

    if (it == tools.end()) {
        callback(jsonResponse(AdminResponse::failJson("工具不存在")));
        co_return;
    }

    const auto &tool = *it;

    // 构建导出 JSON（不包含内部 ID）
    json exportJson;
    exportJson["name"] = tool.name;
    exportJson["description"] = tool.description;

    // 解析参数 JSON
    json params;
    std::ignore = tryParseJson(tool.parameters, params);
    exportJson["parameters"] = params;

    exportJson["scriptContent"] = tool.scriptContent;
    exportJson["executorType"] = tool.executorType;
    if (!tool.readme.empty()) {
        exportJson["readme"] = tool.readme;
    }
    exportJson["version"] = "1.0";

    // 返回 JSON 文件
    const std::string content = dumpJson(exportJson, true, 2);

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + tool.name + ".json\"");
    resp->setBody(content);
    callback(resp);
    co_return;
}

auto AdminController::importCustomTool(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto body = parseJsonBody(req);
    if (!body) {
        callback(jsonResponse(AdminResponse::failJson("无效的 JSON 数据")));
        co_return;
    }

    // 检查必要字段
    if (!body->contains("name") || !body->contains("description") || !body->contains("scriptContent")) {
        callback(jsonResponse(AdminResponse::failJson("缺少必要字段：name, description, scriptContent")));
        co_return;
    }

    std::string name = getStr(*body, "name");

    // 检查是否已存在同名工具
    if (ToolStore::hasCustomTool(name) || ToolRegistry::instance().hasTool(name)) {
        callback(jsonResponse(AdminResponse::failJson("工具名已存在：" + name)));
        co_return;
    }

    ToolStore::CustomTool tool;
    tool.name = name;
    tool.description = getStr(*body, "description");
    tool.executorType = getStr(*body, "executorType");
    if (tool.executorType != "lua") {
        callback(jsonResponse(AdminResponse::failJson("仅支持导入 Lua 脚本；旧 Python 工具需要手动改写")));
        co_return;
    }

    // 参数处理
    if (body->contains("parameters")) {
        tool.parameters = dumpJson((*body)["parameters"], false);
    } else {
        tool.parameters = R"({"type":"object","properties":{},"required":[]})";
    }

    tool.scriptContent = getStr(*body, "scriptContent");
    if (tool.executorType == "lua") {
        if (const auto error = LuaToolExecutor::validate(tool.scriptContent)) {
            callback(jsonResponse(AdminResponse::failJson(*error)));
            co_return;
        }
    }
    tool.readme = getStr(*body, "readme");
    tool.enabled = true;

    i32 newId = 0;
    try {
        newId = ToolStore::addCustomTool(tool);
    } catch (const std::exception &error) {
        callback(jsonResponse(AdminResponse::failJson(error.what())));
        co_return;
    }
    if (auto result = ToolRuntime::reloadCustomTools(Database::instance()); !result) {
        Logger::error(0, "Tool", result.error());
        auto response =
          jsonResponse(AdminResponse::failJson("工具数据可能已保存，但运行时重载失败: " + result.error()));
        response->setStatusCode(k500InternalServerError);
        callback(response);
        co_return;
    }

    Logger::info(0, "Admin", fmt::format("导入自定义工具: {} (ID: {})", tool.name, newId));

    json resp = AdminResponse::okJson("工具已导入");
    resp["id"] = newId;
    callback(jsonResponse(resp));
    co_return;
}
