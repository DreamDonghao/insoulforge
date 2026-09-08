/// @file HttpUtil.hpp
/// @brief HTTP 请求工具 - 统一记录请求内容并处理异常
/// @author donghao
/// @date 2026-08-22
/// @details 所有出站 HTTP 请求统一走 send()，保证：
///          - 请求前记录方法、完整 URL、请求体（截断）
///          - 捕获地址解析失败、网络异常并记录，返回 nullopt
///          - 避免 API Key 明文泄漏（日志中脱敏）

#pragma once

#include <cstdint>
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <string_view>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::HttpUtil {
    /// @brief 发送 HTTP 请求，记录请求内容与异常
    /// @param tag 日志前缀，如 "[Router]"、"[Executor]"
    /// @param baseUrl 服务器地址（含协议与端口，如 http://127.0.0.1:3001）
    /// @param path 请求路径，如 /v1/chat/completions
    /// @param method HTTP 方法
    /// @param body JSON 请求体（null 表示无 body，例如 GET；按值接管，协程帧持有）
    /// @param bearerToken Bearer 认证 token（空串则不添加 Authorization 头）
    /// @param timeout 超时秒数
    /// @return 响应；网络异常（含地址解析失败、超时）返回 std::nullopt
    drogon::Task<std::optional<drogon::HttpResponsePtr>> send(std::string_view tag, std::string baseUrl,
      std::string path, drogon::HttpMethod method, json body, std::string bearerToken, double timeout,
      std::optional<uint64_t> sessionId = std::nullopt);
} // namespace insoulforge::HttpUtil
