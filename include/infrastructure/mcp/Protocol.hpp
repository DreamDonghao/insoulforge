#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <expected>
#include <nlohmann/json.hpp>

namespace drogonmcp {
    using Json = nlohmann::json;
    using u64 = std::uint64_t;
    using i32 = std::int32_t;

    enum class ErrorKind { Configuration, State, Transport, Http, Protocol, Remote, Limit };

    /// @brief 请求失败；工具自身的 isError 不属于请求失败，保留在结果 JSON 中。
    struct Error {
        ErrorKind kind;
        std::string message;
        i32 code = 0; ///< HTTP 状态码或 JSON-RPC 错误码，无对应值时为 0。
        Json data;
    };

    template<typename T>
    using Result = std::expected<T, Error>;

    /// @brief 解析完整 HTTP 响应中的 JSON 或有限 SSE，并校验响应 ID。
    /// @note SSE 通知会跳过；服务器主动请求明确报错，不静默丢弃。
    [[nodiscard]] auto decodeResponse(std::string_view body, std::string_view contentType, u64 requestId)
      -> Result<Json>;
} // namespace drogonmcp
