/// @file HttpUtil.cpp
/// @brief HTTP 请求工具 - 实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge::HttpUtil {
    namespace {
        constexpr size_t kBodyLogMax = 400; // 日志中请求体截断长度
        constexpr std::string_view kImageOmitted = "[图片数据已省略]";

        auto methodName(const drogon::HttpMethod m) -> const char * {
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

        auto truncate(std::string s, const size_t max) -> std::string {
            if (s.size() <= max)
                return s;
            return s.substr(0, max) + "…(截断)";
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

    auto redactImagePayloads(const json &value) -> json {
        if (value.is_object()) {
            auto result = json::object();
            for (const auto &[key, item]: value.items()) {
                if (key == "b64_json" && item.is_string()) {
                    result[key] = std::string(kImageOmitted);
                } else {
                    result[key] = redactImagePayloads(item);
                }
            }
            return result;
        }
        if (value.is_array()) {
            auto result = json::array();
            for (const auto &item: value) {
                result.push_back(redactImagePayloads(item));
            }
            return result;
        }
        if (value.is_string()) {
            const auto &content = value.get_ref<const std::string &>();
            if (content.find(";base64,") != std::string::npos || content.starts_with("base64://") ||
                (content.size() > 1024 && content.size() % 4 == 0 &&
                  content.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
                    std::string::npos)) {
                return std::string(kImageOmitted);
            }
        }
        return value;
    }

    auto send(const std::string_view tag, std::string baseUrl, std::string path, const drogon::HttpMethod method,
      json body, std::string bearerToken, const f64 timeout, std::optional<u64> sessionId, const bool traceResponse)
      -> drogon::Task<std::optional<drogon::HttpResponsePtr>> {
        normalizeTarget(baseUrl, path);
        // 实际请求保留原始 JSON；调试记录只省略其中的图片数据。
        auto bodyText = body.is_null() ? std::string{} : dumpJson(body);
        const bool containsImageData =
          bodyText.find(";base64,") != std::string::npos || bodyText.find("base64://") != std::string::npos;
        const auto traceBodyText = containsImageData ? dumpJson(redactImagePayloads(body)) : bodyText;
        const auto bodyLog = truncate(traceBodyText, kBodyLogMax);

        HttpTraceEntry trace;
        trace.tag = std::string(tag);
        trace.method = methodName(method);
        trace.url = baseUrl + path;
        trace.sessionId = sessionId;
        trace.requestBody = traceBodyText;

        drogon::HttpClientPtr client;
        try {
            client = drogon::HttpClient::newHttpClient(baseUrl);
        } catch (const std::exception &e) {
            Logger::error(sessionId.value_or(0), tag,
              fmt::format(
                "HTTP 创建客户端失败: {} ({} {}{}) body={}", e.what(), methodName(method), baseUrl, path, bodyLog));
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
        const auto finishTrace = [&](const i32 statusCode, std::string responseBody) -> void {
            trace.status = statusCode;
            trace.responseBody = std::move(responseBody);
            HttpTrace::instance().append(std::move(trace));
        };

        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client->sendRequestCoro(req, timeout);
        } catch (const std::exception &e) {
            Logger::error(sessionId.value_or(0), tag,
              fmt::format("HTTP 请求异常: {} ({} {}{}) body={}", e.what(), methodName(method), baseUrl, path, bodyLog));
            finishTrace(0, e.what());
            co_return std::nullopt;
        }

        if (!resp) {
            finishTrace(0, "");
            co_return std::nullopt;
        }

        // HTTP 错误带上目标地址；DNS 和连接异常在前面的异常分支处理。
        if (resp->getStatusCode() >= drogon::k400BadRequest) {
            Logger::warn(sessionId.value_or(0), tag,
              fmt::format("HTTP 响应异常: status={} ({} {}{})", static_cast<i32>(resp->getStatusCode()),
                methodName(method), baseUrl, path));
        }

        if (traceResponse) {
            finishTrace(static_cast<i32>(resp->getStatusCode()), std::string{resp->body()});
        } else {
            json responseJson;
            finishTrace(static_cast<i32>(resp->getStatusCode()), tryParseJson(resp->body(), responseJson)
                                                                   ? dumpJson(redactImagePayloads(responseJson))
                                                                   : "[非 JSON 响应已省略]");
        }

        co_return resp;
    }
} // namespace insoulforge::HttpUtil
