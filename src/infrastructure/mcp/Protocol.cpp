#include <limits>
#include <optional>
#include <vector>

#include <infrastructure/mcp/Protocol.hpp>

namespace drogonmcp {
    namespace {
        auto protocolError(std::string message) -> std::unexpected<Error> {
            return std::unexpected(Error{ErrorKind::Protocol, std::move(message)});
        }
    } // namespace

    auto decodeResponse(std::string_view body, std::string_view contentType, u64 requestId) -> Result<Json> {
        auto mediaType = contentType.substr(0, contentType.find(';'));
        while (!mediaType.empty() && (mediaType.back() == ' ' || mediaType.back() == '\t'))
            mediaType.remove_suffix(1);
        auto messages = std::vector<Json>{};
        if (mediaType == "application/json") {
            messages.push_back(Json::parse(body, nullptr, false));
        } else if (mediaType == "text/event-stream") {
            auto data = std::string{};
            // SSE 使用空行分隔事件，同一事件的多行 data 以换行拼接。
            while (!body.empty()) {
                const auto end = body.find('\n');
                auto line = body.substr(0, end);
                body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
                if (line.ends_with('\r'))
                    line.remove_suffix(1);
                if (line.empty()) {
                    if (!data.empty())
                        messages.push_back(Json::parse(data, nullptr, false));
                    data.clear();
                } else if (line.starts_with("data:")) {
                    line.remove_prefix(5);
                    if (line.starts_with(' '))
                        line.remove_prefix(1);
                    if (!data.empty())
                        data += '\n';
                    data += line;
                }
            }
            if (!data.empty())
                return protocolError("SSE event is missing its terminating blank line");
        } else {
            return protocolError("Unsupported response Content-Type");
        }

        auto result = std::optional<Json>{};
        for (auto &message: messages) {
            if (!message.is_object() || message.value("jsonrpc", Json{}) != "2.0")
                return protocolError("Invalid JSON-RPC envelope");
            if (message.contains("method")) {
                if (!message["method"].is_string() || message.contains("id") || message.contains("result") ||
                    message.contains("error"))
                    return protocolError("Server-initiated requests are not supported in this version");
                continue;
            }
            if (!message.contains("id") || !message["id"].is_number_unsigned() || message["id"].get<u64>() != requestId)
                return protocolError("Response ID does not match request");
            if (result)
                return protocolError("Duplicate JSON-RPC response");
            if (message.contains("result") == message.contains("error"))
                return protocolError("Response must contain exactly one of result or error");
            result = std::move(message);
        }
        if (!result)
            return protocolError("No response for this request");
        if (result->contains("error")) {
            const auto &error = (*result)["error"];
            if (!error.is_object() || !error.contains("code") || !error["code"].is_number_integer() ||
                !error.contains("message") || !error["message"].is_string())
                return protocolError("Invalid JSON-RPC error");
            if (error["code"].is_number_unsigned() &&
                error["code"].get<u64>() > static_cast<u64>(std::numeric_limits<i32>::max()))
                return protocolError("Error code is out of range");
            const auto code = error["code"].get<std::int64_t>();
            if (code < std::numeric_limits<i32>::min() || code > std::numeric_limits<i32>::max())
                return protocolError("Error code is out of range");
            return std::unexpected(Error{ErrorKind::Remote, error["message"].get<std::string>(), static_cast<i32>(code),
              error.value("data", Json{})});
        }
        return std::move((*result)["result"]);
    }
} // namespace drogonmcp
