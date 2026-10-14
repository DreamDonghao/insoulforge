#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include <agent/tools/ToolRegistry.hpp>
#include <agent/tools/mcp/McpTools.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        auto toolError(std::string message) -> std::string { return json{{"error", std::move(message)}}.dump(); }

        auto lowerAscii(std::string text) -> std::string {
            for (auto &c: text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            return text;
        }

        auto summary(std::string text) -> std::string {
            auto limit = std::min(text.size(), size_t{512});
            if (limit < text.size())
                while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80)
                    --limit;
            text.resize(limit);
            return text;
        }
        auto isToolName(std::string_view name) -> bool {
            return !name.empty() && std::ranges::all_of(name, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                       c == '-';
            });
        }

        auto describeError(const drogonmcp::Error &error) -> std::string {
            auto result = fmt::format("{} (code={})", error.message, error.code);
            if (!error.data.is_null())
                result += " | data=" + error.data.dump();
            return result;
        }

        /// 关闭失败不能覆盖已成功执行的工具结果；异常也只记录为清理警告。
        auto closeSession(drogonmcp::Client &client, u64 sessionId) -> drogon::Task<> {
            try {
                auto closed = co_await client.close();
                if (!closed)
                    Logger::warn(sessionId, "MCP", "会话关闭失败：" + describeError(closed.error()));
            } catch (const std::exception &error) {
                Logger::warn(sessionId, "MCP", std::string("会话关闭异常：") + error.what());
            }
        }

        auto queryTools(drogonmcp::Client &client) -> drogon::Task<drogonmcp::Result<std::vector<drogonmcp::Json>>> {
            auto initialized = co_await client.initialize();
            if (!initialized)
                co_return std::unexpected(initialized.error());
            co_return co_await client.listTools();
        }

        // 配置和参数由协程帧持有；每次调用独立会话，可同时服务不同群聊。
        auto callRemote(drogonmcp::ClientOptions options, std::string remoteName, json arguments,
          ToolCallContext context) -> drogon::Task<std::string> {
            auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
            if (!loop)
                co_return "MCP 工具失败：当前线程没有事件循环";
            auto client = drogonmcp::Client{*loop, std::move(options)};
            auto output = std::string{};
            try {
                auto initialized = co_await client.initialize();
                if (!initialized)
                    output = "MCP 初始化失败：" + describeError(initialized.error());
                else {
                    auto result = co_await client.callTool(std::move(remoteName), drogonmcp::Json(arguments));
                    if (!result)
                        output = "MCP 请求失败：" + describeError(result.error());
                    else
                        output = result->dump(); // 保留 content、structuredContent 和 isError，交给模型处理。
                }
            } catch (const std::exception &error) {
                output = std::string("MCP 工具异常：") + error.what();
            }
            co_await closeSession(client, context.sessionId);
            co_return output;
        }
    } // namespace

    auto McpTools::instance() -> McpTools & {
        static auto tools = McpTools{};
        return tools;
    }

    auto McpTools::initialize(json config, ToolRegistry &registry) -> std::expected<void, std::string> {
        auto servers = parseConfig(config);
        if (!servers)
            return std::unexpected(servers.error());
        m_registry = std::ref(registry);
        if (!registerManagementTools())
            return std::unexpected("MCP 管理工具注册失败");
        applyServers(std::move(*servers));
        return {};
    }

    auto McpTools::parseConfig(const json &config) -> std::expected<std::vector<std::shared_ptr<Server>>, std::string> {
        if (!config.is_object() || !config.contains("servers") || !config["servers"].is_array())
            return std::unexpected("mcp.servers 必须是数组");
        if (config["servers"].size() > 16)
            return std::unexpected("MCP 服务最多配置 16 个");
        auto servers = std::vector<std::shared_ptr<Server>>{};
        auto names = std::set<std::string>{};
        for (const auto &value: config["servers"]) {
            if (!value.is_object())
                return std::unexpected("每个 MCP 服务必须是对象");
            if (value.contains("enabled") && !value["enabled"].is_boolean())
                return std::unexpected("MCP enabled 必须是布尔值");
            if (!value.contains("name") || !value["name"].is_string() || !value.contains("url") ||
                !value["url"].is_string())
                return std::unexpected("MCP 服务必须填写字符串 name 和 url");
            const auto name = value["name"].get<std::string>();
            const auto url = value["url"].get<std::string>();
            if (!isToolName(name) || name.size() > 20 || !names.insert(name).second)
                return std::unexpected("MCP 服务名称必须唯一，由 1 至 20 个英文、数字、下划线或连字符组成");
            if ((!url.starts_with("http://") && !url.starts_with("https://")) || url.size() > 4096)
                return std::unexpected("MCP url 必须是 HTTP(S) 地址且不超过 4096 字节");
            if (value.contains("token") && !value["token"].is_string())
                return std::unexpected("MCP token 必须是字符串");
            if (value.contains("description") && (!value["description"].is_string() ||
                                                   value["description"].get_ref<const std::string &>().size() > 1024))
                return std::unexpected("MCP description 必须是字符串且不超过 1024 UTF-8 字节");
            if (value.contains("timeoutSeconds") && !value["timeoutSeconds"].is_number())
                return std::unexpected("MCP timeoutSeconds 必须是数值");
            const auto timeout = value.value("timeoutSeconds", 30.0);
            if (!std::isfinite(timeout) || timeout < 1 || timeout > 120)
                return std::unexpected("MCP timeoutSeconds 必须在 1 至 120 秒之间");
            auto options = drogonmcp::ClientOptions{
              .endpoint = url,
              .name = "insoulforge",
              .bearerToken = value.value("token", std::string{}),
              .timeoutSeconds = timeout,
              .maxResponseBytes = 1024 * 1024,
              .maxToolCount = 256,
              .maxPages = 20,
            };
            if (auto valid = drogonmcp::validateOptions(options); !valid)
                return std::unexpected("MCP 服务 " + name + " 配置无效：" + valid.error().message);
            if (!value.value("enabled", true))
                continue;
            servers.push_back(std::make_shared<Server>(Server{
              .name = name, .description = value.value("description", std::string{}), .options = std::move(options)}));
        }
        return servers;
    }

    void McpTools::applyServers(std::vector<std::shared_ptr<Server>> servers) {
        const auto lock = std::scoped_lock{m_mutex};
        for (auto &server: servers) {
            const auto old = std::ranges::find_if(m_servers, [&](const auto &entry) {
                return entry->name == server->name && entry->options.endpoint == server->options.endpoint &&
                       entry->options.bearerToken == server->options.bearerToken &&
                       entry->options.timeoutSeconds == server->options.timeoutSeconds;
            });
            if (old != m_servers.end()) {
                (*old)->description = std::move(server->description);
                server = *old;
            } else
                server->generation = m_nextVersion++;
        }
        for (const auto &old: m_servers) {
            if (!std::ranges::contains(servers, old) && m_registry)
                m_registry->get().unregisterPlugin("mcp:" + old->name);
        }
        m_servers = std::move(servers);
    }

    auto McpTools::saveConfig(json config, Config &store) -> std::expected<void, ConfigError> {
        const auto lock = std::scoped_lock{m_configurationMutex};
        auto servers = parseConfig(config);
        if (!servers)
            return std::unexpected(ConfigError{.type = ConfigErrorType::InvalidArgument, .message = servers.error()});
        // 完整列表保存需要防止旧页面覆盖其他管理员刚修改的服务。
        if (config.contains("originalServers") && config["originalServers"] != store.getMcpConfig()["servers"])
            return std::unexpected(ConfigError{
              .type = ConfigErrorType::Conflict, .message = "MCP 配置已被其他页面修改，请重新加载页面后再保存"});
        auto normalized = config["servers"];
        for (auto &server: normalized)
            if (!server.contains("description"))
                server["description"] = "";
        if (auto saved = store.saveMcpConfig(json{{"servers", std::move(normalized)}}); !saved)
            return saved;
        applyServers(std::move(*servers));
        return {};
    }

    auto McpTools::status() -> json {
        const auto lock = std::scoped_lock{m_mutex};
        auto result = json::array();
        for (const auto &server: m_servers)
            result.push_back({{"name", server->name}, {"loaded", server->loaded}, {"loading", server->loading},
              {"toolCount", server->catalog ? server->catalog->tools.size() : 0}, {"error", server->error}});
        return result;
    }

    auto McpTools::refreshTools() -> drogon::Task<> {
        {
            const auto lock = std::scoped_lock{m_mutex};
            for (auto &server: m_servers) {
                // 替换状态对象使旧发现结果失效，防止删除或修改后的服务被重新注册。
                auto next = std::make_shared<Server>(
                  Server{.name = server->name, .description = server->description, .options = server->options});
                next->generation = server->generation;
                next->catalog = server->catalog;
                server = std::move(next);
            }
        }
        co_await ensureTools();
    }

    auto McpTools::discover(std::shared_ptr<Server> server) -> drogon::Task<std::expected<void, std::string>> {
        auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!loop)
            co_return std::unexpected("当前线程没有事件循环");
        auto client = drogonmcp::Client{*loop, server->options};
        // 查询抛出异常时也清理会话；配置与网络错误仍保留具体原因。
        auto tools = drogonmcp::Result<std::vector<drogonmcp::Json>>{};
        try {
            tools = co_await queryTools(client);
        } catch (const std::exception &error) {
            tools = std::unexpected(drogonmcp::Error{drogonmcp::ErrorKind::Protocol, error.what()});
        }
        co_await closeSession(client, 0);
        if (!tools)
            co_return std::unexpected(describeError(tools.error()));

        auto definitions = std::vector<Tool>{};
        definitions.reserve(tools->size());
        for (const auto &tool: *tools) {
            const auto remoteName = tool.at("name").get<std::string>();
            const auto localName = "mcp_" + server->name + "_" + remoteName;
            if (!isToolName(remoteName) || localName.size() > 64 ||
                (tool.contains("description") && !tool["description"].is_string()) ||
                tool["inputSchema"].value("type", json{}) != "object")
                co_return std::unexpected(
                  "MCP 工具名称、描述或参数 Schema 不符合模型 function calling 要求：" + remoteName);
            definitions.push_back(Tool{
              .name = localName,
              .description = "[MCP " + server->name + "] " + tool.value("description", remoteName),
              .parameters = tool["inputSchema"],
              .handler = [options = server->options, remoteName](
                           json arguments, ToolCallContext context) -> drogon::Task<std::string> {
                  co_return co_await callRemote(options, remoteName, std::move(arguments), std::move(context));
              },
            });
        }
        const auto lock = std::scoped_lock{m_mutex};
        if (!std::ranges::contains(m_servers, server))
            co_return std::unexpected("服务配置已变更，忽略旧发现结果");
        auto toolDefinitions = json(*tools);
        const auto version =
          server->catalog && server->catalog->tools == toolDefinitions ? server->catalog->version : m_nextVersion++;
        for (auto &definition: definitions)
            definition.mcpVersion = version;
        const auto registered =
          m_registry->get().registerPlugin("mcp:" + server->name, [&definitions](ToolRegistry &registry) {
              for (const auto &tool: definitions)
                  std::ignore = registry.registerTool(tool, ToolCategory::ACTION);
          });
        if (!registered)
            co_return std::unexpected("MCP 工具注册失败，原有工具未被覆盖");
        server->catalog = std::make_shared<Catalog>(Catalog{.version = version, .tools = std::move(toolDefinitions)});
        Logger::info(0, "MCP", fmt::format("服务 {} 工具加载完成 | count={}", server->name, definitions.size()));
        co_return std::expected<void, std::string>{};
    }

    auto McpTools::ensureTools() -> drogon::Task<> {
        auto snapshot = std::vector<std::shared_ptr<Server>>{};
        {
            const auto lock = std::scoped_lock{m_mutex};
            if (!m_registry)
                co_return;
            snapshot = m_servers;
        }
        for (const auto &server: snapshot) {
            auto result = co_await loadServer(server);
            if (!result)
                Logger::warn(0, "MCP", "服务 " + server->name + " 工具加载失败：" + result.error());
        }
    }

    auto McpTools::loadServer(std::shared_ptr<Server> server) -> drogon::Task<std::expected<void, std::string>> {
        {
            const auto lock = std::scoped_lock{m_mutex};
            if (!std::ranges::contains(m_servers, server))
                co_return std::unexpected("服务配置已变更，请开始新的回复");
            if (server->loaded)
                co_return std::expected<void, std::string>{};
            if (server->loading)
                co_return std::unexpected("服务正在加载，本轮不要重复查询，可稍后再使用");
            if (std::chrono::steady_clock::now() < server->retryAfter)
                co_return std::unexpected(server->error);
            server->loading = true;
        }
        auto result = std::expected<void, std::string>{};
        try {
            result = co_await discover(server);
        } catch (const std::exception &error) {
            result = std::unexpected(error.what());
        }
        const auto lock = std::scoped_lock{m_mutex};
        if (!std::ranges::contains(m_servers, server))
            co_return std::unexpected("服务配置已变更，忽略旧发现结果");
        server->loading = false;
        server->loaded = result.has_value();
        server->error = result ? std::string{} : result.error();
        server->retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        co_return result;
    }

    auto McpTools::directoryLocked() const -> json {
        auto entries = std::map<std::string, std::string>{};
        for (const auto &server: m_servers)
            entries.emplace(server->name, server->description);
        auto result = json::array();
        for (const auto &[name, description]: entries) {
            auto entry = json{{"name", name}};
            if (!description.empty())
                entry["description"] = description;
            result.push_back(std::move(entry));
        }
        return result;
    }

    auto McpTools::directory() -> json {
        const auto lock = std::scoped_lock{m_mutex};
        return directoryLocked();
    }

    auto McpTools::createSelection(const json &executionConfig) -> std::shared_ptr<McpToolSelection> {
        auto selection = std::make_shared<McpToolSelection>();
        selection->limits = {.pageSize = getInt(executionConfig, "mcpToolPageSize", 10),
          .maxPerEnable = getInt(executionConfig, "mcpMaxToolsPerEnable", 4),
          .maxPerReply = getInt(executionConfig, "mcpMaxToolsPerReply", 12)};
        const auto lock = std::scoped_lock{m_mutex};
        selection->directory = directoryLocked();
        for (const auto &server: m_servers)
            selection->services.emplace(server->name, server->generation);
        return selection;
    }

    auto McpTools::registerManagementTools() -> bool {
        return m_registry->get().registerPlugin("builtin.mcp", [this](ToolRegistry &registry) {
            const auto server = json{{"type", "string"}, {"description", "服务目录中的名称"}};
            std::ignore = registry.registerTool(
              Tool{
                .name = "mcp_list_tools",
                .description = "查询选定 MCP 服务的工具名称与用途。query 为可选关键词，cursor "
                               "为上一页返回的游标。结果不含参数定义，需要调用 mcp_enable_tools "
                               "后才能执行具体工具。加载中或失败时不要连续重复查询。",
                .parameters = {{"type", "object"},
                  {"properties",
                    {{"server", server}, {"query", {{"type", "string"}}}, {"cursor", {{"type", "string"}}}}},
                  {"required", json::array({"server"})}},
                .handler = [this](json args, ToolCallContext context) -> drogon::Task<std::string> {
                    co_return co_await listTools(std::move(args), std::move(context.mcpSelection));
                },
                .mcpManagement = true,
              },
              ToolCategory::INFORMATION);
            std::ignore = registry.registerTool(
              Tool{
                .name = "mcp_enable_tools",
                .description = "按服务和远程工具名称启用当前回复所需的工具。整批校验，重复启用不占额外名额，数量限制见"
                               "执行状态。成功后下一轮才会开放完整定义，不要在本轮直接调用新启用工具。",
                .parameters = {{"type", "object"},
                  {"properties", {{"server", server},
                                   {"names", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}}}}},
                  {"required", json::array({"server", "names"})}},
                .handler = [this](json args, ToolCallContext context) -> drogon::Task<std::string> {
                    co_return enableTools(args, *context.mcpSelection);
                },
                .mcpManagement = true,
              },
              ToolCategory::INFORMATION);
        });
    }

    auto McpTools::listTools(json arguments, std::shared_ptr<McpToolSelection> selection) -> drogon::Task<std::string> {
        if (!selection || !arguments.is_object() || !arguments.contains("server") || !arguments["server"].is_string())
            co_return toolError("需要当前回复的选择状态和字符串 server");
        for (const auto *key: {"query", "cursor"})
            if (arguments.contains(key) && !arguments[key].is_string())
                co_return toolError("query 和 cursor 必须是字符串");
        const auto name = arguments["server"].get<std::string>();
        const auto query = lowerAscii(arguments.value("query", std::string{}));
        const auto cursor = arguments.value("cursor", std::string{});
        if (query.size() > 1024 || cursor.size() > 4096)
            co_return toolError("查询关键词或游标过长");
        auto server = std::shared_ptr<Server>{};
        {
            const auto lock = std::scoped_lock{m_mutex};
            for (const auto &entry: m_servers)
                if (entry->name == name && selection->services.contains(name) &&
                    selection->services.at(name) == entry->generation)
                    server = entry;
        }
        if (!server)
            co_return toolError("服务不在本次回复目录中、已禁用或连接配置已修改");
        bool cached = false;
        {
            const auto lock = std::scoped_lock{m_mutex};
            cached = bool(server->catalog);
        }
        if (!cached) {
            auto loaded = co_await loadServer(server);
            if (!loaded)
                co_return toolError(loaded.error());
        }
        auto catalog = std::shared_ptr<const Catalog>{};
        {
            const auto lock = std::scoped_lock{m_mutex};
            if (!std::ranges::contains(m_servers, server))
                co_return toolError("服务已修改，请开始新的回复");
            catalog = server->catalog;
        }
        auto matches = json::array();
        for (const auto &tool: catalog->tools) {
            const auto remoteName = tool["name"].get<std::string>();
            const auto description = tool.value("description", std::string{});
            if (query.empty() || lowerAscii(remoteName + "\n" + description).find(query) != std::string::npos)
                matches.push_back({{"name", remoteName}, {"description", summary(description)}});
        }
        size_t offset = 0;
        if (!cursor.empty()) {
            const auto page = json::parse(cursor, nullptr, false);
            if (!page.is_object() || atOrNull(page, "version") != catalog->version ||
                atOrNull(page, "query") != query || !atOrNull(page, "offset").is_number_unsigned() ||
                page["offset"] > matches.size())
                co_return toolError("游标无效或工具缓存已变化，请不带游标重新查询");
            offset = page["offset"].get<size_t>();
        }
        const auto end = std::min(matches.size(), offset + static_cast<size_t>(selection->limits.pageSize));
        auto page = json::array();
        for (auto index = offset; index < end; ++index)
            page.push_back(std::move(matches[index]));
        auto result = json{{"server", name}, {"tools", std::move(page)}, {"total", matches.size()}};
        if (end < matches.size())
            result["nextCursor"] = json{{"version", catalog->version}, {"offset", end}, {"query", query}}.dump();
        co_return result.dump();
    }

    auto McpTools::enableTools(const json &arguments, McpToolSelection &selection) -> std::string {
        if (!arguments.is_object() || !atOrNull(arguments, "server").is_string() ||
            !atOrNull(arguments, "names").is_array())
            return toolError("需要字符串 server 和工具名称数组 names");
        auto names = std::set<std::string>{};
        for (const auto &name: arguments["names"]) {
            if (!name.is_string() || name.get_ref<const std::string &>().empty())
                return toolError("工具名称必须是非空字符串");
            names.insert(name.get<std::string>());
        }
        if (names.empty() || names.size() > static_cast<size_t>(selection.limits.maxPerEnable))
            return toolError("单次启用数量超限：" + std::to_string(selection.limits.maxPerEnable));
        const auto service = arguments["server"].get<std::string>();
        const auto lock = std::scoped_lock{m_mutex};
        auto server = std::shared_ptr<Server>{};
        auto versions = std::set<u64>{};
        for (const auto &entry: m_servers) {
            if (entry->catalog)
                versions.insert(entry->catalog->version);
            if (entry->name == service && selection.services.contains(service) &&
                selection.services.at(service) == entry->generation)
                server = entry;
        }
        if (!server || !server->catalog)
            return toolError("服务不可用或工具未加载，请先查询该服务");
        auto candidate = selection.enabled;
        std::erase_if(candidate, [&](const auto &entry) { return !versions.contains(entry.second); });
        auto enabled = json::array();
        for (const auto &name: names) {
            const auto found =
              std::ranges::find_if(server->catalog->tools, [&](const auto &tool) { return tool["name"] == name; });
            if (found == server->catalog->tools.end())
                return toolError("未知工具：" + name + "，整批未启用");
            const auto localName = "mcp_" + service + "_" + name;
            candidate[localName] = server->catalog->version;
            enabled.push_back(localName);
        }
        if (candidate.size() > static_cast<size_t>(selection.limits.maxPerReply))
            return toolError(
              "本次回复最多启用 " + std::to_string(selection.limits.maxPerReply) + " 个 MCP 工具，整批未启用");
        selection.enabled = std::move(candidate);
        return json{{"enabledTools", std::move(enabled)}, {"totalEnabled", selection.enabled.size()},
          {"availableFrom", "下一轮模型请求"}}
          .dump();
    }
} // namespace insoulforge
