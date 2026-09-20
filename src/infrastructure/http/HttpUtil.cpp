/// @file HttpUtil.cpp
/// @brief HTTP 请求工具 - 实现

#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

namespace insoulforge::HttpUtil {
    namespace {
        constexpr size_t kBodyLogMax = 400; // 日志中请求体截断长度

        const char *methodName(const drogon::HttpMethod m) {
            switch (m) {
                case drogon::Get:
                    return "GET";
                case drogon::Post:
                    return "POST";
                case drogon::Put:
                    return "PUT";
                case drogon::Delete:
                    return "DELETE";
                case drogon::Options:
                    return "OPTIONS";
                case drogon::Patch:
                    return "PATCH";
                case drogon::Head:
                    return "HEAD";
                default:
                    return "?";
            }
        }

        std::string truncate(std::string s, const size_t max) {
            if (s.size() <= max)
                return s;
            return s.substr(0, max) + "…(截断)";
        }

        // 脱敏：只保留头尾各 4 位，避免 token 明文进日志
        std::string maskToken(const std::string_view token) {
            if (token.empty())
                return "(空)";
            if (token.size() <= 8)
                return std::string(token.size(), '*');
            return std::string(token.substr(0, 4)) + "****" + std::string(token.substr(token.size() - 4));
        }

        /// @brief 将带路径前缀的 Base URL 拆为 Drogon 所需的主机地址和完整请求路径。
        /// @details HttpClient::newHttpClient() 不接受路径；例如将
        ///          https://api.example.com/v1 与 /chat/completions 转为
        ///          https://api.example.com 和 /v1/chat/completions。
        void normalizeTarget(std::string &baseUrl, std::string &path) {
            const size_t schemeEnd = baseUrl.find("://");
            if (schemeEnd == std::string::npos)
                return;

            const size_t prefixStart = baseUrl.find('/', schemeEnd + 3);
            if (prefixStart == std::string::npos)
                return;

            std::string prefix = baseUrl.substr(prefixStart);
            baseUrl.erase(prefixStart);
            if (path.empty()) {
                path = std::move(prefix);
                return;
            }
            if (path.front() != '/')
                path.insert(path.begin(), '/');
            while (prefix.size() > 1 && prefix.back() == '/')
                prefix.pop_back();
            path = prefix == "/" ? std::move(path) : std::move(prefix) + path;
        }
    } // namespace

    drogon::Task<std::optional<drogon::HttpResponsePtr>> send(const std::string_view tag, std::string baseUrl,
      std::string path, const drogon::HttpMethod method, json body, std::string bearerToken, const double timeout,
      std::optional<uint64_t> sessionId) {
        const auto prefix = sessionId.has_value() ? fmt::format("[group_id={}] {}", *sessionId, tag) : std::string(tag);
        normalizeTarget(baseUrl, path);
        // 请求体完整序列化一次：请求、HttpTrace（全量）、日志（截断）共用
        auto bodyText = body.is_null() ? std::string{} : dumpJson(body);
        const auto bodyLog = truncate(bodyText, kBodyLogMax);

        spdlog::debug("{} [HTTP] {} {}{}", prefix, methodName(method), baseUrl, path);
        if (!bodyLog.empty()) {
            spdlog::debug("{} [HTTP] body={}", prefix, bodyLog);
        }
        if (!bearerToken.empty()) {
            spdlog::debug("{} [HTTP] Authorization: Bearer {}", prefix, maskToken(bearerToken));
        }

        HttpTraceEntry trace;
        trace.tag = std::string(tag);
        trace.method = methodName(method);
        trace.url = baseUrl + path;
        trace.sessionId = sessionId;

        drogon::HttpClientPtr client;
        try {
            client = drogon::HttpClient::newHttpClient(baseUrl);
        } catch (const std::exception &e) {
            spdlog::error("{} [HTTP] 创建客户端失败: {} ({} {}{}) body={}", prefix, e.what(), methodName(method),
              baseUrl, path, bodyLog);
            trace.status = 0;
            trace.responseBody = e.what();
            HttpTrace::instance().append(std::move(trace));
            co_return std::nullopt;
        }

        const auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        if (!bodyText.empty()) {
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            req->setBody(bodyText);
        }
        if (!bearerToken.empty()) {
            req->addHeader("Authorization", "Bearer " + bearerToken);
        }
        trace.requestBody = std::move(bodyText);

        const auto finishTrace = [&](const int statusCode, std::string responseBody) {
            trace.status = statusCode;
            trace.responseBody = std::move(responseBody);
            HttpTrace::instance().append(std::move(trace));
        };

        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client->sendRequestCoro(req, timeout);
        } catch (const std::exception &e) {
            spdlog::error(
              "{} [HTTP] 请求异常: {} ({} {}{}) body={}", prefix, e.what(), methodName(method), baseUrl, path, bodyLog);
            finishTrace(0, e.what());
            co_return std::nullopt;
        }

        if (!resp) {
            finishTrace(0, "");
            co_return std::nullopt;
        }

        // 非 2xx（如 DNS 解析失败、连接被拒等）同样把地址打出来，方便定位
        if (resp->getStatusCode() >= drogon::k400BadRequest) {
            spdlog::warn("{} [HTTP] 响应异常: status={} ({} {}{})", prefix, static_cast<int>(resp->getStatusCode()),
              methodName(method), baseUrl, path);
        }

        finishTrace(static_cast<int>(resp->getStatusCode()), std::string{resp->body()});

        co_return resp;
    }
} // namespace insoulforge::HttpUtil
