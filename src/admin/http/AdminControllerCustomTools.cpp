/// @file AdminControllerCustomTools.cpp
/// @brief 管理后台 REST API 控制器 - LLM 配置 / 提示词 / 自定义工具部分的实现
/// @author donghao
/// @date 2026-09-01

#include <infrastructure/NumericTypes.hpp>

#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/ToolStore.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/PromptStore.hpp>

using namespace insoulforge;
using namespace drogon;

// ==================== LLM配置 ====================

auto AdminController::getLLMConfigs(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto configs = ConfigStore::getAllLLMConfigs();
    callback(jsonResponse(configs));
    co_return;
}

auto AdminController::saveLLMConfig(
  const HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("name")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少name字段")));
        co_return;
    }

    const std::string name = getStr(*body, "name");
    ConfigStore::saveLLMConfig(name, *body);

    // 更新内存中的配置
    struct ConfigTarget {
        std::string_view name;
        LLMApiConfig *api;
        LLMModelParams *params; // nullptr 表示该配置没有模型参数
        i32 defaultMaxTokens;
    };
    auto &config = Config::instance();
    const std::array targets{
      ConfigTarget{.name = "router", .api = &config.router, .params = &config.routerParams, .defaultMaxTokens = 100},
      ConfigTarget{
        .name = "executor", .api = &config.executor, .params = &config.executorParams, .defaultMaxTokens = 100},
      ConfigTarget{.name = "executorThinking",
        .api = &config.executorThinking,
        .params = &config.executorThinkingParams,
        .defaultMaxTokens = 512},
      ConfigTarget{.name = "image", .api = &config.image, .params = &config.imageParams, .defaultMaxTokens = 1024},
      ConfigTarget{.name = "embedding", .api = &config.embedding, .params = nullptr, .defaultMaxTokens = 0},
    };

    if (const auto it = std::ranges::find(targets, name, &ConfigTarget::name); it != targets.end()) {
        auto &api = *it->api;
        api.apiKey = getStr(*body, "apiKey");
        api.baseUrl = getStr(*body, "baseUrl");
        api.path = getStr(*body, "path");
        api.model = getStr(*body, "model");
        api.reasoningEffort = getStr(*body, "reasoningEffort");
        if (it->params) {
            it->params->maxTokens = getInt(*body, "maxTokens", it->defaultMaxTokens);
            it->params->temperature = getDouble(*body, "temperature", 0.7);
            it->params->topP = getDouble(*body, "topP", getDouble(*body, "top_P", 0.9));
        }
    }

    callback(jsonResponse(AdminResponse::okJson("LLM配置已保存")));
    co_return;
}

// ==================== 提示词 ====================

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
    auto body = parseJsonBody(req);
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

auto AdminController::getCustomTools(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto tools = ToolStore::getCustomTools();

    json result(json::array());
    for (const auto &tool: tools) {
        json item;
        item["id"] = tool.id;
        item["name"] = tool.name;
        item["description"] = tool.description;
        item["parameters"] = tool.parameters;
        item["executorType"] = tool.executorType;
        item["executorConfig"] = tool.executorConfig;
        item["scriptContent"] = tool.scriptContent;
        item["readme"] = tool.readme;
        item["enabled"] = tool.enabled;
        result.push_back(item);
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::addCustomTool(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("name") || !body->contains("executorType") || !body->contains("executorConfig")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少必要字段 (name, executorType, executorConfig)")));
        co_return;
    }

    const std::string name = getStr(*body, "name");

    // 检查是否与内置工具名冲突
    const auto &registry = ToolRegistry::instance();
    if (registry.hasTool(name)) {
        callback(jsonResponse(AdminResponse::errorJson("工具名 '" + name + "' 已存在（内置工具或自定义工具）")));
        co_return;
    }

    ToolStore::CustomTool tool;
    tool.name = name;
    tool.description = getStr(*body, "description");
    tool.parameters = getStr(*body, "parameters");
    tool.executorType = getStr(*body, "executorType");
    tool.executorConfig = getStr(*body, "executorConfig");
    tool.scriptContent = getStr(*body, "scriptContent");
    tool.readme = getStr(*body, "readme");
    tool.enabled = getBool(*body, "enabled", true);

    const i32 id = ToolStore::addCustomTool(tool);

    // 立即注册到 ToolRegistry
    ToolRuntime::reloadCustomTools();

    json resp = AdminResponse::okJson("自定义工具已添加");
    resp["id"] = id;
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::updateCustomTool(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("name") || !body->contains("executorType") || !body->contains("executorConfig")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少必要字段 (name, executorType, executorConfig)")));
        co_return;
    }

    ToolStore::CustomTool tool;
    tool.id = std::stoi(id);
    tool.name = getStr(*body, "name");
    tool.description = getStr(*body, "description");
    tool.parameters = getStr(*body, "parameters");
    tool.executorType = getStr(*body, "executorType");
    tool.executorConfig = getStr(*body, "executorConfig");
    tool.scriptContent = getStr(*body, "scriptContent");
    tool.readme = getStr(*body, "readme");
    tool.enabled = getBool(*body, "enabled", true);

    ToolStore::updateCustomTool(tool);

    // 重新注册工具
    ToolRuntime::reloadCustomTools();

    callback(jsonResponse(AdminResponse::okJson("自定义工具已更新")));
    co_return;
}

auto AdminController::deleteCustomTool(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const i32 toolId = std::stoi(id);
    ToolStore::deleteCustomTool(toolId);

    // 重新注册工具（移除已删除的）
    ToolRuntime::reloadCustomTools();

    callback(jsonResponse(AdminResponse::okJson("自定义工具已删除")));
    co_return;
}

auto AdminController::toggleCustomTool(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const i32 toolId = std::stoi(id);
    ToolStore::toggleCustomTool(toolId);

    // 重新注册工具
    ToolRuntime::reloadCustomTools();

    callback(jsonResponse(AdminResponse::okJson("工具状态已切换")));
    co_return;
}

auto AdminController::reloadCustomTools(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    ToolRuntime::reloadCustomTools();

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
    std::string executorConfig;
    std::string scriptContent;
    json testArgs;

    if (body->contains("toolId")) {
        // 从数据库加载工具
        const i32 toolId = getInt(*body, "toolId");
        auto tools = ToolStore::getCustomTools();
        auto it = std::ranges::find_if(tools, [toolId](const auto &t) -> auto { return t.id == toolId; });
        if (it == tools.end()) {
            callback(jsonResponse(AdminResponse::failJson("工具不存在")));
            co_return;
        }
        executorType = it->executorType;
        executorConfig = it->executorConfig;
        scriptContent = it->scriptContent;
        testArgs = body->contains("args") ? (*body)["args"] : json();
    } else {
        // 直接使用传入的定义
        executorType = getStr(*body, "executorType", "python");
        executorConfig = getStr(*body, "executorConfig");
        scriptContent = getStr(*body, "scriptContent");
        testArgs = body->contains("args") ? (*body)["args"] : json();
    }

    std::string result;
    if (executorType == "python") {
        result = co_await ToolRuntime::executePythonTool(std::move(scriptContent), std::move(testArgs));
    } else if (executorType == "http") {
        result = co_await ToolRuntime::executeHttpTool(std::move(executorConfig), std::move(testArgs), 0);
    } else {
        result = "未知的执行类型";
    }

    json resp = AdminResponse::okJson();
    resp["result"] = result;
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 自定义工具配置 ====================

auto AdminController::getCustomToolConfig(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    json resp;
    resp["pythonPath"] = ToolStore::getCustomToolPython();
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::saveCustomToolConfig(
  const HttpRequestPtr req, const std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("pythonPath")) {
        callback(jsonResponse(AdminResponse::failJson("缺少 pythonPath 字段")));
        co_return;
    }

    const std::string pythonPath = getStr(*body, "pythonPath");
    ToolStore::setCustomToolPython(pythonPath);

    callback(jsonResponse(AdminResponse::okJson("Python解释器路径已保存")));
    co_return;
}

// ============== 自定义工具导入导出 ==============

auto AdminController::exportCustomTool(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    i32 toolId = std::stoi(id);
    auto tools = ToolStore::getCustomTools();

    auto it = std::ranges::find_if(tools, [toolId](const ToolStore::CustomTool &t) -> bool { return t.id == toolId; });

    if (it == tools.end()) {
        callback(jsonResponse(AdminResponse::failJson("工具不存在")));
        co_return;
    }

    const auto &tool = *it;

    // 只支持导出 Python 工具
    if (tool.executorType != "python") {
        callback(jsonResponse(AdminResponse::failJson("仅支持导出 Python 类型工具")));
        co_return;
    }

    // 构建导出 JSON（简化格式，不含 executorType）
    json exportJson;
    exportJson["name"] = tool.name;
    exportJson["description"] = tool.description;

    // 解析参数 JSON
    json params;
    std::ignore = tryParseJson(tool.parameters, params);
    exportJson["parameters"] = params;

    exportJson["scriptContent"] = tool.scriptContent;
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
    if (ToolStore::hasCustomTool(name)) {
        callback(jsonResponse(AdminResponse::failJson("工具名已存在：" + name)));
        co_return;
    }

    // 构建工具对象（强制使用 Python 类型）
    ToolStore::CustomTool tool;
    tool.name = name;
    tool.description = getStr(*body, "description");
    tool.executorType = "python";

    // 参数处理
    if (body->contains("parameters")) {
        tool.parameters = dumpJson((*body)["parameters"], false);
    } else {
        tool.parameters = R"({"type":"object","properties":{},"required":[]})";
    }

    tool.scriptContent = getStr(*body, "scriptContent");
    tool.readme = getStr(*body, "readme");
    tool.enabled = true;

    // 添加到数据库
    i32 newId = ToolStore::addCustomTool(tool);
    ToolRuntime::reloadCustomTools();

    Logger::info(0, "Admin", fmt::format("导入自定义工具: {} (ID: {})", tool.name, newId));

    json resp = AdminResponse::okJson("工具已导入");
    resp["id"] = newId;
    callback(jsonResponse(resp));
    co_return;
}
