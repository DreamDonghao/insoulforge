#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>

#include <infrastructure/mcp/Protocol.hpp>

namespace drogonmcp {
    struct ClientOptions {
        std::string endpoint;
        std::string name = "drogon-mcp";
        std::string version = "0.1.0";
        std::string bearerToken;
        std::map<std::string, std::string> headers;
        double timeoutSeconds = 30.0;
        size_t maxResponseBytes = 8 * 1024 * 1024;
        size_t maxToolCount = 10000;
        size_t maxPages = 100;
        /// 同步观察 HTTP 请求结果；字符串视图仅在回调期间有效，不包含认证请求头。
        /// status 为 0 表示未收到响应，回调异常不会改变请求结果。
        std::function<void(drogon::HttpMethod, std::string_view, i32, std::string_view)> onHttpTrace;
    };

    /// @brief 在保存配置前校验地址、认证和请求限制，不连接网络。
    [[nodiscard]] auto validateOptions(const ClientOptions &options) -> Result<void>;

    /// @brief 单个 MCP HTTP 会话的客户端，不创建或运行事件循环。
    /// @note 所有操作必须在传入的 loop 上串行调用并 co_await 到完成。
    /// 客户端和 loop 必须存活到操作完成。并发请求明确返回 State 错误。
    class Client {
    public:
        /// @brief 构造不连接网络；配置错误在 initialize 中返回。
        Client(trantor::EventLoop &loop, ClientOptions options);
        Client(const Client &) = delete;
        auto operator=(const Client &) -> Client & = delete;

        /// @brief 完成 initialize 和 initialized 通知，返回服务器初始化结果。
        auto initialize() -> drogon::Task<Result<Json>>;
        /// @brief 自动遍历分页；保留服务器返回的完整工具定义。
        auto listTools() -> drogon::Task<Result<std::vector<Json>>>;
        /// @brief 不重试调用，避免有副作用的工具被重复执行。
        auto callTool(std::string name, Json arguments = Json::object()) -> drogon::Task<Result<Json>>;
        /// @brief DELETE 远程会话；405 视为服务器不支持主动关闭，本地仍关闭。
        auto close() -> drogon::Task<Result<void>>;
        [[nodiscard]] auto isInitialized() const noexcept -> bool;

    private:
        auto exchange(Json message, bool notification = false) -> drogon::Task<Result<Json>>;
        auto send(drogon::HttpMethod method, std::string body) -> drogon::Task<Result<drogon::HttpResponsePtr>>;
        auto checkOperation() const -> Result<void>;
        void traceHttp(drogon::HttpMethod method, std::string_view requestBody, i32 status,
          std::string_view responseBody) const noexcept;

        trantor::EventLoop &m_loop;
        ClientOptions m_options;
        drogon::HttpClientPtr m_http;
        std::string m_origin;
        std::string m_path;
        std::string m_sessionId;
        std::string m_protocolVersion;
        Json m_capabilities = Json::object();
        u64 m_nextId = 1;
        bool m_initialized = false;
        bool m_busy = false;
        std::string m_configurationError;
    };
} // namespace drogonmcp
