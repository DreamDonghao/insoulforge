#include <atomic>
#include <infrastructure/logging/Logger.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <admin/http/AdminController.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/mcp/McpTools.hpp>
#include <workflow/message/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/mcp/Client.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>

namespace {
    using namespace drogonmcp;

    struct FixtureState {
        std::atomic<size_t> listRequests{0};
        std::atomic<size_t> authenticatedRequests{0};
        std::atomic_bool deleteDuringList{false};
        std::atomic_bool duplicateTools{false};
        std::atomic_bool failClose{false};
        std::atomic_bool changedSchema{false};
    };

    void require(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    auto runTests(trantor::EventLoop &loop, std::string endpoint, int &exitCode, FixtureState &fixture)
      -> drogon::Task<> {
        try {
            auto options = ClientOptions{.endpoint = endpoint, .headers = {{"X-API-Key", "fixture-key"}}};
            auto exchanges = std::vector<insoulforge::HttpTraceEntry>{};
            options.onHttpTrace = [&](drogon::HttpMethod method, std::string_view request, i32 status,
                                    std::string_view response) {
                exchanges.push_back({.method = method == drogon::Delete ? "DELETE" : "POST",
                  .requestBody = std::string{request},
                  .status = status,
                  .responseBody = std::string{response}});
                throw std::runtime_error("trace observer failure");
            };
            require(validateOptions(options).has_value(), "Custom API key header rejected");
            for (const auto &headers:
              std::vector<std::map<std::string, std::string>>{{{"X-API-Key", "one"}, {"x-api-key", "two"}},
                {{"Bad Header", "value"}}, {{"X-API-Key", "bad\r\nvalue"}}, {{"Content-Type", "text/plain"}}}) {
                auto invalidOptions = options;
                invalidOptions.headers = headers;
                require(!validateOptions(invalidOptions), "Invalid custom header accepted");
            }
            auto client = Client{loop, options};
            require(!(co_await client.listTools()), "Uninitialized call must fail");
            auto initialized = co_await client.initialize();
            require(initialized.has_value(), initialized ? "" : initialized.error().message.c_str());
            auto tools = co_await client.listTools();
            require(tools && tools->size() == 2, "Pagination failed");
            fixture.duplicateTools.store(true);
            auto duplicate = co_await client.listTools();
            require(!duplicate && duplicate.error().kind == ErrorKind::Protocol, "Duplicate tool names accepted");
            fixture.duplicateTools.store(false);
            auto result = co_await client.callTool("echo", drogonmcp::Json{{"text", "hello"}});
            require(result && (*result)["content"][0]["text"] == "hello", "SSE tool response failed");
            auto toolError = co_await client.callTool("failure");
            require(toolError && (*toolError)["isError"] == true, "Tool error must remain a successful RPC result");
            auto remote = co_await client.callTool("remote");
            require(!remote && remote.error().kind == ErrorKind::Remote && remote.error().code == -32602,
              "Remote error was lost");
            auto upstream = co_await client.callTool("http_error");
            require(!upstream && upstream.error().code == 502 &&
                      upstream.error().message.find("fixture unavailable") != std::string::npos,
              "HTTP error detail was lost");
            require((co_await client.close()).has_value(), "Close failed");
            require(exchanges.front().status == 200 &&
                      drogonmcp::Json::parse(exchanges.front().requestBody)["method"] == "initialize" &&
                      exchanges.back().method == "DELETE" && exchanges.back().status == 405,
              "HTTP observer missed initialization or close");
            require(std::ranges::any_of(exchanges,
                      [](const auto &entry) {
                          return entry.status == 502 &&
                                 entry.responseBody.find("fixture unavailable") != std::string::npos;
                      }),
              "HTTP observer lost upstream error response");
            require(fixture.authenticatedRequests.load() >= 4, "Custom header not sent across MCP operations");
            require(!client.isInitialized(), "Close did not reset state");

            auto slowOptions = options;
            slowOptions.timeoutSeconds = 0.05;
            auto slow = Client{loop, std::move(slowOptions)};
            require((co_await slow.initialize()).has_value(), "Slow client initialize failed");
            auto timeout = co_await slow.callTool("slow");
            require(!timeout && timeout.error().kind == ErrorKind::Transport, "Timeout did not fail");
            require(
              exchanges.back().status == 0 && !exchanges.back().responseBody.empty(), "Transport error was not traced");
            require((co_await slow.close()).has_value(), "Timeout client close failed");
            auto invalid = Client{loop, {.endpoint = "file:///tmp/test"}};
            auto bad = co_await invalid.initialize();
            require(!bad && bad.error().kind == ErrorKind::Configuration, "Invalid URL accepted");

            auto &registry = insoulforge::ToolRegistry::instance();
            auto &mcp = insoulforge::McpTools::instance();
            auto &traces = insoulforge::HttpTrace::instance();
            traces.clear();
            require(
              !mcp.initialize(drogonmcp::Json{{"servers", "invalid"}}, registry), "Invalid MCP configuration accepted");
            require(!mcp.initialize(drogonmcp::Json{{"servers", drogonmcp::Json::array({drogonmcp::Json{
                                                                  {"name", "invalid name"}, {"url", endpoint}}})}},
                      registry),
              "Invalid MCP name accepted");
            for (const auto &headers:
              drogonmcp::Json::array({"invalid", {{"X-API-Key", 123}}, {{"X-API-Key", "one"}, {"x-api-key", "two"}}})) {
                require(!mcp.initialize(drogonmcp::Json{{"servers", drogonmcp::Json::array({{{"name", "fixture"},
                                                                      {"url", endpoint}, {"headers", headers}}})}},
                          registry),
                  "Invalid MCP adapter headers accepted");
            }
            auto configured = mcp.initialize(
              drogonmcp::Json{
                {"servers", drogonmcp::Json::array({drogonmcp::Json{{"name", "fixture"}, {"url", endpoint},
                                                      {"token", ""}, {"headers", {{"X-API-Key", "fixture-key"}}}},
                              drogonmcp::Json{{"name", "disabled"}, {"url", endpoint}, {"enabled", false}},
                              drogonmcp::Json{{"name", "unused"}, {"url", endpoint}}})}},
              registry);
            require(configured.has_value(), "MCP adapter configuration failed");
            require(registry.getTools({}).empty(), "MCP tools leaked into default model request");
            auto selection =
              mcp.createSelection({{"mcpToolPageSize", 1}, {"mcpMaxToolsPerEnable", 4}, {"mcpMaxToolsPerReply", 12}});
            require(selection->enabled.empty() && selection->directory[0].value("description", std::string{}) == "",
              "Old description default missing");
            const auto prompt = insoulforge::ExecutorAgent::buildSystemPrompt(
              insoulforge::SessionId::fromPrivateUser(42), selection->directory);
            require(prompt.find("mcp_list_tools") != std::string::npos && prompt.find(endpoint) == std::string::npos &&
                      prompt.find("inputSchema") == std::string::npos,
              "Directory prompt exposed connection or full tool definitions");
            const auto beforeQuery = fixture.listRequests.load();
            auto page = insoulforge::json::parse(co_await registry.executeTool(
              "mcp_list_tools", {{"server", "fixture"}}, {.sessionId = 42, .mcpSelection = selection}));
            require(
              page["tools"].size() == 1 && page.contains("nextCursor") && !page["tools"][0].contains("inputSchema"),
              "MCP listing did not paginate summaries");
            require(page["tools"][0]["description"].get<std::string>().size() == 510, "UTF-8 summary was split");
            require(fixture.listRequests.load() == beforeQuery + 2 && !mcp.status()[1]["loaded"].get<bool>(),
              "Listing one service queried another service");
            const auto cursor = page["nextCursor"].get<std::string>();
            auto secondPage =
              insoulforge::json::parse(co_await mcp.listTools({{"server", "fixture"}, {"cursor", cursor}}, selection));
            require(
              secondPage["tools"][0]["name"] == "second" && !secondPage.contains("nextCursor"), "Next page failed");
            auto search =
              insoulforge::json::parse(co_await mcp.listTools({{"server", "fixture"}, {"query", "ECHO"}}, selection));
            require(search["total"] == 1, "Case-insensitive search failed");
            auto emptySearch = insoulforge::json::parse(
              co_await mcp.listTools({{"server", "fixture"}, {"query", "not-found"}}, selection));
            require(emptySearch["tools"].empty() && emptySearch["total"] == 0, "Empty search incorrect");
            auto beforeEnable = selection->enabled;
            auto enabled = insoulforge::json::parse(co_await registry.executeTool("mcp_enable_tools",
              {{"server", "fixture"}, {"names", {"echo", "echo"}}}, {.sessionId = 42, .mcpSelection = selection}));
            require(enabled["enabledTools"].size() == 1 && selection->enabled.size() == 1, "Enable/dedup failed");
            auto unauthorized = co_await registry.executeTool(
              "mcp_fixture_echo", {}, {.sessionId = 42, .mcpSelection = selection, .offeredMcpTools = beforeEnable});
            require(
              unauthorized.find("未在本轮开放") != std::string::npos, "Same-round activation bypassed authorization");
            const auto selectedDefinitions = registry.getTools({.mcpTools = selection->enabled, .mcpManagement = true});
            require(selectedDefinitions.size() == 3, "Next round did not expose only helpers and selected tool");
            require(
              registry.getTools({.replyOnly = true, .mcpTools = selection->enabled, .mcpManagement = true}).empty(),
              "Last round exposed MCP helpers or remote tools");
            auto lastRoundHelper = co_await registry.executeTool(
              "mcp_enable_tools", {}, {.sessionId = 42, .replyOnly = true, .mcpSelection = selection});
            require(lastRoundHelper.find("最后一轮") != std::string::npos, "Last round helper executed");
            auto otherReply = mcp.createSelection({});
            require(otherReply->enabled.empty() && registry.getTools({.mcpTools = otherReply->enabled}).empty(),
              "Reply/group selection leaked");
            auto failedBatch = insoulforge::json::parse(
              mcp.enableTools({{"server", "fixture"}, {"names", {"second", "unknown"}}}, *selection));
            require(
              failedBatch.contains("error") && selection->enabled.size() == 1, "Unknown tool partially enabled batch");
            auto limited = mcp.createSelection({{"mcpMaxToolsPerEnable", 1}, {"mcpMaxToolsPerReply", 1}});
            require(insoulforge::json::parse(
                      mcp.enableTools({{"server", "fixture"}, {"names", {"echo", "second"}}}, *limited))
                        .contains("error") &&
                      limited->enabled.empty(),
              "Per-enable limit ignored");
            std::ignore = mcp.enableTools({{"server", "fixture"}, {"names", {"echo"}}}, *limited);
            require(insoulforge::json::parse(mcp.enableTools({{"server", "fixture"}, {"names", {"second"}}}, *limited))
                        .contains("error") &&
                      limited->enabled.size() == 1,
              "Total limit ignored");
            require(insoulforge::json::parse(
                      mcp.enableTools({{"server", "fixture"}, {"names", {"echo"}}}, *limited))["totalEnabled"] == 1,
              "Repeated enable consumed slots");
            co_await mcp.ensureTools();
            require(registry.hasTool("mcp_fixture_echo") && registry.hasTool("mcp_fixture_second"),
              "Remote tools were not registered");
            const auto requestsAfterDiscovery = fixture.listRequests.load();
            co_await mcp.ensureTools();
            require(fixture.listRequests.load() == requestsAfterDiscovery, "Cached tools were queried again");
            fixture.failClose.store(true);
            auto remoteOutput = co_await registry.executeTool("mcp_fixture_echo", drogonmcp::Json{{"text", "hello"}},
              {.sessionId = 42, .mcpSelection = selection, .offeredMcpTools = selection->enabled});
            auto parsedOutput = drogonmcp::Json::parse(remoteOutput, nullptr, false);
            require(parsedOutput.is_object() && parsedOutput["content"][0]["text"] == "hello",
              "Registered MCP tool did not execute");
            const auto recorded = traces.query(0, 50);
            require(std::ranges::any_of(recorded,
                      [&](const auto &entry) {
                          const auto request = drogonmcp::Json::parse(entry.requestBody, nullptr, false);
                          return entry.tag == "MCP:fixture" && entry.url == endpoint && entry.sessionId == 42 &&
                                 entry.status == 200 && request.is_object() &&
                                 request.value("method", "") == "tools/call" &&
                                 entry.responseBody.find("hello") != std::string::npos;
                      }),
              "MCP tool call missing from frontend trace cache");
            require(std::ranges::any_of(recorded,
                      [](const auto &entry) {
                          return entry.tag == "MCP:fixture" && !entry.sessionId &&
                                 entry.requestBody.find("tools/list") != std::string::npos;
                      }),
              "MCP discovery missing from trace cache");
            require(std::ranges::none_of(recorded,
                      [](const auto &entry) {
                          return entry.requestBody.find("fixture-key") != std::string::npos ||
                                 entry.responseBody.find("fixture-key") != std::string::npos;
                      }),
              "Authentication header leaked into trace cache");
            auto blocked = co_await registry.executeTool(
              "mcp_fixture_echo", drogonmcp::Json::object(), {.sessionId = 42, .replyOnly = true});
            require(blocked.find("最后一轮") != std::string::npos, "MCP tool bypassed reply-only restriction");
            require(registry.getTools({.replyOnly = true}).empty(), "Remote tools included in last round");
            auto &config = insoulforge::Config::instance();
            auto serviceConfig = insoulforge::json{{"servers",
              insoulforge::json::array({insoulforge::json{{"name", "fixture"}, {"description", ""}, {"url", endpoint},
                {"token", ""}, {"headers", {{"X-API-Key", "fixture-key"}}}, {"enabled", true}}})}};
            require(mcp.saveConfig(serviceConfig, config).has_value(), "Dynamic save failed");
            require(config.getMcpConfig() == serviceConfig, "MCP configuration not persisted");
            auto described = serviceConfig;
            described["servers"][0]["description"] = "查询项目资料";
            const auto requestsBeforeDescription = fixture.listRequests.load();
            require(mcp.saveConfig(described, config).has_value(), "Description save failed");
            co_await mcp.ensureTools();
            require(fixture.listRequests.load() == requestsBeforeDescription &&
                      selection->directory[0].value("description", std::string{}) == "" &&
                      mcp.directory()[0]["description"] == "查询项目资料",
              "Description update reset cache or mutated frozen prompt");
            require(mcp.saveConfig(serviceConfig, config).has_value(), "Description reset failed");
            auto badDescription = serviceConfig;
            badDescription["servers"][0]["description"] = std::string(1025, 'x');
            require(!mcp.saveConfig(badDescription, config), "Oversized description accepted");
            fixture.duplicateTools.store(true);
            co_await mcp.refreshTools();
            require(registry.getTools({.mcpTools = selection->enabled}).size() == 1,
              "Failed refresh invalidated old tool version");
            fixture.duplicateTools.store(false);
            co_await mcp.refreshTools();
            require(registry.getTools({.mcpTools = selection->enabled}).size() == 1,
              "Unchanged refresh invalidated selected tool");
            fixture.changedSchema.store(true);
            co_await mcp.refreshTools();
            require(registry.getTools({.mcpTools = selection->enabled}).empty(),
              "Old selection survived changed tool catalog");
            auto staleCursor =
              insoulforge::json::parse(co_await mcp.listTools({{"server", "fixture"}, {"cursor", cursor}}, selection));
            require(staleCursor.contains("error"), "Old pagination cursor survived refresh");
            auto staleCall = co_await registry.executeTool("mcp_fixture_echo", {},
              {.sessionId = 42, .mcpSelection = selection, .offeredMcpTools = selection->enabled});
            require(staleCall.find("定义已更新") != std::string::npos, "Old tool authorization survived refresh");
            std::ignore = mcp.enableTools({{"server", "fixture"}, {"names", {"echo"}}}, *selection);
            require(registry.getTools({.mcpTools = selection->enabled}).size() == 1, "Re-enable after refresh failed");
            require(!mcp.saveConfig(insoulforge::json{{"servers", false}}, config), "Invalid dynamic save accepted");
            require(registry.hasTool("mcp_fixture_echo") && config.getMcpConfig() == serviceConfig,
              "Invalid save changed tools or persisted configuration");
            auto stale = insoulforge::json{
              {"servers", insoulforge::json::array()}, {"originalServers", insoulforge::json::array()}};
            auto conflict = mcp.saveConfig(stale, config);
            require(!conflict && conflict.error().type == insoulforge::ConfigErrorType::Conflict &&
                      config.getMcpConfig() == serviceConfig,
              "Stale page overwrote current configuration");
            auto malformed = serviceConfig;
            malformed["servers"][0]["url"] = "https://";
            require(!mcp.saveConfig(malformed, config) && config.getMcpConfig() == serviceConfig,
              "Malformed endpoint was persisted");
            require(mcp.status()[0]["toolCount"] == 2 && mcp.status()[0]["loaded"] == true,
              "Tool status does not reflect loaded tools");
            auto disabled = serviceConfig;
            disabled["servers"][0]["enabled"] = false;
            require(mcp.saveConfig(disabled, config).has_value() && !registry.hasTool("mcp_fixture_echo"),
              "Disabled service tools were not unloaded");
            require(mcp.saveConfig(serviceConfig, config).has_value(), "Re-enable failed");
            co_await mcp.ensureTools();
            require(registry.hasTool("mcp_fixture_echo"), "Re-enabled tools were not discovered");
            fixture.deleteDuringList.store(true);
            co_await mcp.refreshTools();
            require(mcp.status().empty() && !registry.hasTool("mcp_fixture_echo"),
              "In-flight discovery restored a removed service");
            auto noServices = mcp.createSelection({});
            require(noServices->directory.empty() &&
                      registry.getTools({.mcpManagement = !noServices->services.empty()}).empty(),
              "Helpers exposed without services");
            const auto promptWithoutMcp = insoulforge::ExecutorAgent::buildSystemPrompt(
              insoulforge::SessionId::fromPrivateUser(42), noServices->directory);
            require(
              promptWithoutMcp.find("## 可用 MCP 服务") == std::string::npos, "Empty directory injected into prompt");
            require(insoulforge::json::parse(mcp.enableTools({{"server", "fixture"}, {"names", {"echo"}}}, *selection))
                      .contains("error"),
              "Deleted service still enabled");
            auto controller = insoulforge::AdminController{};
            auto response = drogon::HttpResponsePtr{};
            const auto callback = [&response](const drogon::HttpResponsePtr &value) { response = value; };
            co_await controller.getMcpConfig(drogon::HttpRequest::newHttpRequest(), callback);
            auto configuration = drogonmcp::Json::parse(response->body());
            require(configuration["servers"].empty() && configuration["status"].empty() &&
                      response->getHeader("cache-control") == "no-store",
              "MCP GET response invalid");
            auto invalidRequest = drogon::HttpRequest::newHttpRequest();
            invalidRequest->setBody(R"({"servers":"invalid"})");
            co_await controller.saveMcpConfig(invalidRequest, callback);
            require(response->statusCode() == drogon::k400BadRequest, "Invalid MCP HTTP save accepted");
            co_await controller.refreshMcpTools(drogon::HttpRequest::newHttpRequest(), callback);
            require(drogonmcp::Json::parse(response->body())["success"] == true, "Refresh endpoint failed");
            std::cout << "All client tests passed\n";
            exitCode = 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
        loop.quit();
    }
} // namespace

auto main() -> int {
    using namespace drogonmcp;
    require(decodeResponse(R"({"jsonrpc":"2.0","id":1,"result":{}})", "application/json", 1).has_value(),
      "JSON parse failed");
    require(!decodeResponse(R"({"jsonrpc":"2.0","id":2,"result":{}})", "application/json", 1), "Wrong ID accepted");
    require(!decodeResponse("data: invalid\n\n", "text/event-stream", 1), "Malformed SSE accepted");
    require(
      !decodeResponse(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json", 1), "Server request accepted");
    require(!decodeResponse("data: {}", "text/event-stream", 1), "Incomplete SSE accepted");
    require(
      !decodeResponse(R"({"jsonrpc":"2.0","id":1,"result":{}})", "application/jsonp", 1), "Invalid MIME type accepted");
    require(decodeResponse(R"({"jsonrpc":"2.0","id":1,"result":{}})", "application/json; charset=utf-8", 1).has_value(),
      "JSON charset rejected");
    for (const auto &endpoint:
      {"https://", "http://user:password@host/mcp", "http://host:0/mcp", "http://host:65536/mcp", "http://host:abc/mcp",
        "http://host\\evil/mcp", "http://host/mcp#fragment", "http://[xx::1]/mcp"}) {
        require(!validateOptions({.endpoint = endpoint}), "Invalid endpoint accepted");
    }
    require(validateOptions({.endpoint = "http://[::1]:8080/mcp"}).has_value(), "IPv6 endpoint rejected");
    require(!validateOptions({.endpoint = "http://localhost/mcp", .bearerToken = "token\r\nvalue"}),
      "Invalid token accepted");

    drogon::app().setLogLevel(trantor::Logger::kError);
    drogon::app().addListener("127.0.0.1", 0);
    auto fixture = FixtureState{};
    const auto configPath =
      std::filesystem::temp_directory_path() /
      ("insoulforge-mcp-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    require(
      insoulforge::Config::instance().initialize(configPath.string()).has_value(), "Test config initialize failed");
    require(insoulforge::Database::instance().initialize(":memory:").has_value(), "Test database initialize failed");
    insoulforge::PromptService::initialize(insoulforge::Database::instance());
    drogon::app().registerHandler("/mcp",
      [&fixture](
        const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          auto response = drogon::HttpResponse::newHttpResponse();
          if (request->getHeader("x-api-key") == "fixture-key")
              ++fixture.authenticatedRequests;
          if (request->method() == drogon::Delete) {
              response->setStatusCode(
                fixture.failClose.exchange(false) ? drogon::k500InternalServerError : drogon::k405MethodNotAllowed);
              callback(response);
              return;
          }
          auto input = drogonmcp::Json::parse(request->body());
          const auto method = input.at("method").get<std::string>();
          if (method == "notifications/initialized") {
              require(request->getHeader("mcp-session-id") == "test-session", "Missing session header");
              require(request->getHeader("mcp-protocol-version") == "2025-11-25", "Missing protocol header");
              response->setStatusCode(drogon::k202Accepted);
              callback(response);
              return;
          }
          auto output = drogonmcp::Json{{"jsonrpc", "2.0"}, {"id", input["id"]}};
          if (method == "initialize") {
              require(input["params"]["capabilities"].empty(), "Unsupported capabilities advertised");
              response->addHeader("MCP-Session-Id", "test-session");
              output["result"] = drogonmcp::Json{{"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", drogonmcp::Json::object()}}},
                {"serverInfo", {{"name", "fixture"}, {"version", "1"}}}};
          } else if (method == "tools/list") {
              ++fixture.listRequests;
              if (fixture.deleteDuringList.exchange(false)) {
                  require(insoulforge::McpTools::instance()
                            .saveConfig(insoulforge::json{{"servers", insoulforge::json::array()}},
                              insoulforge::Config::instance())
                            .has_value(),
                    "Delete during discovery failed");
              }
              const auto second = input["params"].contains("cursor");
              output["result"] = drogonmcp::Json{{"tools",
                drogonmcp::Json::array(
                  {drogonmcp::Json{{"name", second && !fixture.duplicateTools.load() ? "second" : "echo"},
                    {"description", std::string(510, 'x') + "中文说明"}, {"inputSchema", {{"type", "object"}}}}})}};
              if (!second)
                  output["result"]["nextCursor"] = "next";
              if (fixture.changedSchema.load()) {
                  output["result"]["tools"][0]["inputSchema"]["properties"] =
                    drogonmcp::Json{{"text", {{"type", "string"}}}};
              }
          } else {
              const auto name = input["params"]["name"].get<std::string>();
              if (name == "slow")
                  return; // 不调用回调，验证客户端超时，不启动后台任务。
              if (name == "http_error") {
                  response->setStatusCode(drogon::k502BadGateway);
                  response->setContentTypeString("application/json");
                  response->setBody(R"({"error":{"message":"fixture unavailable"}})");
                  callback(response);
                  return;
              }
              if (name == "remote")
                  output["error"] = drogonmcp::Json{{"code", -32602}, {"message", "Invalid arguments"}};
              else
                  output["result"] = drogonmcp::Json{
                    {"content", drogonmcp::Json::array({drogonmcp::Json{{"type", "text"}, {"text", "hello"}}})},
                    {"isError", name == "failure"}};
              if (name == "echo") {
                  response->setContentTypeString("text/event-stream");
                  response->setBody(": heartbeat\r\n\r\ndata: "
                                    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\r\n\r\ndata: " +
                                    output.dump() + "\r\n\r\n");
                  callback(response);
                  return;
              }
          }
          response->setContentTypeString("application/json");
          response->setBody(output.dump());
          callback(response);
      },
      {drogon::Post, drogon::Delete});
    auto clientThread = trantor::EventLoopThread{};
    auto exitCode = 1;
    clientThread.run();
    drogon::app().registerBeginningAdvice([&clientThread, &exitCode, &fixture] {
        const auto port = drogon::app().getListeners().front().toPort();
        clientThread.getLoop()->queueInLoop([&clientThread, &exitCode, &fixture, port] {
            drogon::async_run([&clientThread, &exitCode, &fixture, port]() -> drogon::Task<> {
                co_await runTests(
                  *clientThread.getLoop(), "http://127.0.0.1:" + std::to_string(port) + "/mcp", exitCode, fixture);
            });
        });
    });
    drogon::app().run();
    clientThread.wait();
    std::filesystem::remove(configPath);
    insoulforge::Database::instance().close();
    return exitCode;
}
