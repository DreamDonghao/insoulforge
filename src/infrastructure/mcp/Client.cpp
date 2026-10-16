#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <utility>

#include <infrastructure/mcp/Client.hpp>

namespace drogonmcp {
    namespace {
        constexpr auto kProtocolVersion = "2025-11-25";

        // 整个公共操作持有标记，而不只是单次 HTTP 请求，避免分页或握手被插入其他请求。
        class OperationGuard {
        public:
            explicit OperationGuard(bool &busy) : m_busy(busy) { m_busy = true; }
            ~OperationGuard() { m_busy = false; }
            OperationGuard(const OperationGuard &) = delete;
            auto operator=(const OperationGuard &) -> OperationGuard & = delete;

        private:
            bool &m_busy;
        };

        auto failure(ErrorKind kind, std::string message, i32 code = 0) -> std::unexpected<Error> {
            return std::unexpected(Error{kind, std::move(message), code});
        }

        auto lower(std::string value) -> std::string {
            for (auto &c: value) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            }
            return value;
        }

        auto httpError(const drogon::HttpResponsePtr &response) -> std::unexpected<Error> {
            const auto status = static_cast<i32>(response->statusCode());
            auto detail = Json::parse(response->body(), nullptr, false);
            auto message = "MCP endpoint returned HTTP " + std::to_string(status);
            if (detail.is_object()) {
                const auto error = detail.find("error");
                if (error != detail.end() && error->is_object() && error->contains("message") &&
                    (*error)["message"].is_string())
                    message += ": " + (*error)["message"].get<std::string>();
            } else
                detail = nullptr;
            return std::unexpected(Error{ErrorKind::Http, std::move(message), status, std::move(detail)});
        }

        auto isVisible(std::string_view value) -> bool {
            return std::ranges::all_of(value, [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
        }

        struct Endpoint {
            std::string origin;
            std::string path;
        };

        auto parseEndpoint(std::string_view url) -> Result<Endpoint> {
            const auto scheme = url.starts_with("https://")  ? size_t{8}
                                : url.starts_with("http://") ? size_t{7}
                                                             : size_t{0};
            if (scheme == 0 || url.find_first_of("#\\") != std::string_view::npos || !isVisible(url))
                return failure(ErrorKind::Configuration,
                  "Endpoint must be an HTTP(S) URL without whitespace, backslash or fragment");
            const auto pathStart = url.find_first_of("/?", scheme);
            const auto origin = url.substr(0, pathStart);
            auto authority = origin.substr(scheme);
            if (authority.empty() || authority.find('@') != std::string_view::npos)
                return failure(ErrorKind::Configuration, "Endpoint requires a host and must not contain credentials");
            auto port = std::string_view{};
            if (authority.starts_with('[')) {
                const auto end = authority.find(']');
                if (end == std::string_view::npos || end < 2 ||
                    authority.substr(1, end - 1).find(':') == std::string_view::npos)
                    return failure(ErrorKind::Configuration, "Invalid bracketed IPv6 host");
                if (authority.substr(1, end - 1).find_first_not_of("0123456789abcdefABCDEF:.") !=
                    std::string_view::npos)
                    return failure(ErrorKind::Configuration, "Invalid characters in IPv6 host");
                const auto suffix = authority.substr(end + 1);
                if (!suffix.empty()) {
                    if (!suffix.starts_with(':'))
                        return failure(ErrorKind::Configuration, "Invalid host suffix");
                    port = suffix.substr(1);
                    if (port.empty())
                        return failure(ErrorKind::Configuration, "Endpoint port is empty");
                }
            } else {
                const auto colon = authority.find(':');
                if (colon != std::string_view::npos) {
                    port = authority.substr(colon + 1);
                    authority = authority.substr(0, colon);
                    if (port.empty())
                        return failure(ErrorKind::Configuration, "Endpoint port is empty");
                }
                if (authority.empty() || !std::ranges::all_of(authority, [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                               c == '-' || c == '_';
                    }))
                    return failure(ErrorKind::Configuration, "Invalid endpoint host; use ASCII or a punycode domain");
            }
            if (!port.empty()) {
                auto number = u64{};
                const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
                if (error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535)
                    return failure(ErrorKind::Configuration, "Endpoint port must be between 1 and 65535");
            }
            auto path = pathStart == std::string_view::npos ? std::string{"/"} : std::string{url.substr(pathStart)};
            if (path.starts_with('?'))
                path.insert(0, "/");
            return Endpoint{std::string{origin}, std::move(path)};
        }
    } // namespace

    auto validateOptions(const ClientOptions &options) -> Result<void> {
        if (auto endpoint = parseEndpoint(options.endpoint); !endpoint)
            return std::unexpected(endpoint.error());
        if (!std::isfinite(options.timeoutSeconds) || options.timeoutSeconds <= 0 || options.maxResponseBytes == 0 ||
            options.maxPages == 0 || options.maxToolCount == 0 || options.name.empty() || options.version.empty())
            return failure(ErrorKind::Configuration, "Timeout, size limits and client identity must be valid");
        if (!isVisible(options.bearerToken))
            return failure(ErrorKind::Configuration, "Bearer token contains invalid characters");
        constexpr auto reserved = std::array{"accept", "content-type", "host", "content-length", "transfer-encoding",
          "mcp-session-id", "mcp-protocol-version"};
        auto headerNames = std::set<std::string>{};
        for (const auto &[name, value]: options.headers) {
            const auto validName = !name.empty() && std::ranges::all_of(name, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       std::string_view{"!#$%&'*+-.^_`|~"}.find(c) != std::string_view::npos;
            });
            const auto validValue =
              std::ranges::all_of(value, [](unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); });
            if (!validName || !validValue || !headerNames.insert(lower(name)).second ||
                std::ranges::contains(reserved, lower(name)) ||
                (lower(name) == "authorization" && !options.bearerToken.empty()))
                return failure(ErrorKind::Configuration, "Invalid or reserved custom HTTP header");
        }
        return {};
    }

    Client::Client(trantor::EventLoop &loop, ClientOptions options) : m_loop(loop), m_options(std::move(options)) {
        if (auto valid = validateOptions(m_options); !valid) {
            m_configurationError = valid.error().message;
            return;
        }
        auto endpoint = *parseEndpoint(m_options.endpoint);
        m_origin = std::move(endpoint.origin);
        m_path = std::move(endpoint.path);
        try {
            m_http = drogon::HttpClient::newHttpClient(m_origin, &m_loop);
            if (!m_http)
                m_configurationError = "Cannot create HTTP client";
        } catch (const std::exception &error) {
            m_configurationError = error.what();
        }
    }

    auto Client::checkOperation() const -> Result<void> {
        if (!m_loop.isInLoopThread())
            return failure(ErrorKind::State, "Call client operations on the supplied event loop");
        if (!m_configurationError.empty())
            return failure(ErrorKind::Configuration, m_configurationError);
        if (m_busy)
            return failure(ErrorKind::State, "Another client operation is still running");
        return {};
    }

    auto Client::isInitialized() const noexcept -> bool { return m_initialized; }

    void Client::traceHttp(drogon::HttpMethod method, std::string_view requestBody, i32 status,
      std::string_view responseBody) const noexcept {
        if (!m_options.onHttpTrace)
            return;
        try {
            m_options.onHttpTrace(method, requestBody, status, responseBody);
        } catch (...) {
            // 调试记录失败不能改变远程调用结果，尤其不能导致有副作用的工具被重试。
        }
    }

    auto Client::send(drogon::HttpMethod method, std::string body) -> drogon::Task<Result<drogon::HttpResponsePtr>> {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        request->setPath(m_path);
        request->setContentTypeString("application/json");
        request->addHeader("Accept", "application/json, text/event-stream");
        for (const auto &[name, value]: m_options.headers)
            request->addHeader(name, value);
        if (!m_options.bearerToken.empty())
            request->addHeader("Authorization", "Bearer " + m_options.bearerToken);
        if (!m_sessionId.empty())
            request->addHeader("MCP-Session-Id", m_sessionId);
        if (!m_protocolVersion.empty())
            request->addHeader("MCP-Protocol-Version", m_protocolVersion);
        request->setBody(std::move(body));
        try {
            auto response = co_await m_http->sendRequestCoro(request, m_options.timeoutSeconds);
            if (!response) {
                traceHttp(method, request->body(), 0, "HTTP request returned no response");
                co_return failure(ErrorKind::Transport, "HTTP request returned no response");
            }
            // 普通 Drogon HTTP 接口在完整接收后才返回；此限制不是接收过程的内存上限。
            if (response->body().size() > m_options.maxResponseBytes) {
                traceHttp(method, request->body(), static_cast<i32>(response->statusCode()),
                  "Response exceeds maxResponseBytes");
                co_return failure(ErrorKind::Limit, "Response exceeds maxResponseBytes");
            }
            traceHttp(method, request->body(), static_cast<i32>(response->statusCode()), response->body());
            co_return response;
        } catch (const drogon::HttpException &error) {
            traceHttp(method, request->body(), 0, error.what());
            // 超时连接可能仍占用请求队列；后续操作使用新连接，不重放当前请求。
            m_http = drogon::HttpClient::newHttpClient(m_origin, &m_loop);
            co_return failure(ErrorKind::Transport, error.what());
        } catch (const std::exception &error) {
            traceHttp(method, request->body(), 0, error.what());
            throw;
        }
    }

    auto Client::exchange(Json message, bool notification) -> drogon::Task<Result<Json>> {
        auto response = co_await send(drogon::Post, message.dump());
        if (!response)
            co_return std::unexpected(response.error());
        const auto status = static_cast<i32>((*response)->statusCode());
        if (status == 404 && !m_sessionId.empty()) {
            m_initialized = false;
            m_sessionId.clear();
            m_protocolVersion.clear();
            co_return failure(ErrorKind::State, "Session expired; initialize again before another call", status);
        }
        if (notification) {
            if (status != 202 || !(*response)->body().empty())
                co_return failure(ErrorKind::Http, "Notification was not accepted with an empty HTTP 202", status);
            co_return Json{};
        }
        if (status != 200)
            co_return httpError(*response);
        auto result =
          decodeResponse((*response)->body(), lower((*response)->getHeader("content-type")), message["id"].get<u64>());
        if (result && message["method"] == "initialize") {
            const auto session = (*response)->getHeader("mcp-session-id");
            if (!isVisible(session))
                co_return failure(ErrorKind::Protocol, "Invalid MCP session ID");
            m_sessionId = session;
        }
        co_return result;
    }

    auto Client::initialize() -> drogon::Task<Result<Json>> {
        auto check = checkOperation();
        if (!check)
            co_return std::unexpected(check.error());
        if (m_initialized)
            co_return failure(ErrorKind::State, "Client is already initialized");
        const auto guard = OperationGuard{m_busy};
        m_sessionId.clear();
        m_protocolVersion.clear();
        auto request = Json{{"jsonrpc", "2.0"}, {"id", m_nextId++}, {"method", "initialize"},
          {"params", {{"protocolVersion", kProtocolVersion}, {"capabilities", Json::object()},
                       {"clientInfo", {{"name", m_options.name}, {"version", m_options.version}}}}}};
        auto result = co_await exchange(std::move(request));
        if (!result)
            co_return result;
        if (!result->is_object() || !result->contains("protocolVersion") || !(*result)["protocolVersion"].is_string() ||
            !result->contains("capabilities") || !(*result)["capabilities"].is_object() ||
            !result->contains("serverInfo") || !(*result)["serverInfo"].is_object())
            co_return failure(ErrorKind::Protocol, "Invalid initialize result");
        const auto &serverInfo = (*result)["serverInfo"];
        if (!serverInfo.contains("name") || !serverInfo["name"].is_string() || !serverInfo.contains("version") ||
            !serverInfo["version"].is_string())
            co_return failure(ErrorKind::Protocol, "Invalid server identity");
        const auto version = (*result)["protocolVersion"].get<std::string>();
        if (version != kProtocolVersion && version != "2025-06-18" && version != "2025-03-26")
            co_return failure(ErrorKind::Protocol, "Unsupported negotiated protocol version: " + version);
        m_protocolVersion = version;
        m_capabilities = (*result)["capabilities"];
        if (m_capabilities.contains("tools") && !m_capabilities["tools"].is_object())
            co_return failure(ErrorKind::Protocol, "Invalid tools capability");
        auto notification = Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
        auto notified = co_await exchange(std::move(notification), true);
        if (!notified)
            co_return std::unexpected(notified.error());
        m_initialized = true;
        co_return result;
    }

    auto Client::listTools() -> drogon::Task<Result<std::vector<Json>>> {
        auto check = checkOperation();
        if (!check)
            co_return std::unexpected(check.error());
        if (!m_initialized || !m_capabilities.contains("tools"))
            co_return failure(ErrorKind::State, "Initialize a server with tools capability first");
        const auto guard = OperationGuard{m_busy};
        auto tools = std::vector<Json>{};
        auto cursors = std::set<std::string>{};
        auto names = std::set<std::string>{};
        auto cursor = std::string{};
        for (size_t page = 0; page < m_options.maxPages; ++page) {
            auto params = Json::object();
            if (!cursor.empty())
                params["cursor"] = cursor;
            auto request =
              Json{{"jsonrpc", "2.0"}, {"id", m_nextId++}, {"method", "tools/list"}, {"params", std::move(params)}};
            auto result = co_await exchange(std::move(request));
            if (!result)
                co_return std::unexpected(result.error());
            if (!result->is_object() || !result->contains("tools") || !(*result)["tools"].is_array())
                co_return failure(ErrorKind::Protocol, "Invalid tools/list result");
            for (const auto &tool: (*result)["tools"]) {
                if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string() ||
                    !tool.contains("inputSchema") || !tool["inputSchema"].is_object())
                    co_return failure(ErrorKind::Protocol, "Invalid tool definition");
                const auto name = tool["name"].get<std::string>();
                if (name.empty() || !names.insert(name).second)
                    co_return failure(ErrorKind::Protocol, "Empty or duplicate tool name: " + name);
                if (tools.size() == m_options.maxToolCount)
                    co_return failure(ErrorKind::Limit, "Too many tools");
                tools.push_back(tool);
            }
            if (!result->contains("nextCursor"))
                co_return tools;
            if (!(*result)["nextCursor"].is_string())
                co_return failure(ErrorKind::Protocol, "Invalid pagination cursor");
            cursor = (*result)["nextCursor"].get<std::string>();
            if (cursor.empty() || !cursors.insert(cursor).second)
                co_return failure(ErrorKind::Protocol, "Empty or repeated pagination cursor");
        }
        co_return failure(ErrorKind::Limit, "Too many pagination pages");
    }

    auto Client::callTool(std::string name, Json arguments) -> drogon::Task<Result<Json>> {
        auto check = checkOperation();
        if (!check)
            co_return std::unexpected(check.error());
        if (!m_initialized || !m_capabilities.contains("tools"))
            co_return failure(ErrorKind::State, "Initialize a server with tools capability first");
        if (name.empty() || !arguments.is_object())
            co_return failure(ErrorKind::Configuration, "Tool name and arguments are invalid");
        const auto guard = OperationGuard{m_busy};
        auto request = Json{{"jsonrpc", "2.0"}, {"id", m_nextId++}, {"method", "tools/call"},
          {"params", {{"name", std::move(name)}, {"arguments", std::move(arguments)}}}};
        auto result = co_await exchange(std::move(request));
        if (result && (!result->is_object() || !result->contains("content") || !(*result)["content"].is_array() ||
                        (result->contains("isError") && !(*result)["isError"].is_boolean())))
            co_return failure(ErrorKind::Protocol, "Invalid tools/call result");
        co_return result;
    }

    auto Client::close() -> drogon::Task<Result<void>> {
        auto check = checkOperation();
        if (!check)
            co_return std::unexpected(check.error());
        const auto guard = OperationGuard{m_busy};
        m_initialized = false;
        if (m_sessionId.empty()) {
            m_protocolVersion.clear();
            co_return Result<void>{};
        }
        auto response = co_await send(drogon::Delete, {});
        m_sessionId.clear();
        m_protocolVersion.clear();
        if (!response)
            co_return std::unexpected(response.error());
        const auto status = static_cast<i32>((*response)->statusCode());
        if ((status < 200 || status >= 300) && status != 405 && status != 404)
            co_return failure(ErrorKind::Http, "Session DELETE failed", status);
        co_return Result<void>{};
    }
} // namespace drogonmcp
