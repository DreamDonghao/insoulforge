/// @file HttpUtil.hpp
/// @brief 出站 HTTP 请求与调试记录
/// @details send() 将请求与响应写入内存调试记录。常规运行日志只记录错误或异常，
///          不记录 Authorization 头；调试记录中的请求体和响应体各有大小上限。

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::HttpUtil {
    /// @brief 发送 HTTP 请求，记录请求内容与异常
    /// @param tag 日志来源和调试记录标签，如 Router、Executor
    /// @param baseUrl 服务 Base URL，可包含路径前缀，如 https://api.example.com/v1
    /// @param path 请求路径，如 /chat/completions；会与 Base URL 中的路径前缀合并
    /// @param method HTTP 方法
    /// @param body JSON 请求体（null 表示无 body，例如 GET；按值接管，协程帧持有）
    /// @param bearerToken Bearer 认证 token（空串则不添加 Authorization 头）
    /// @param timeout 超时秒数
    /// @param sessionId 关联的会话 ID；缺省时视为全局请求
    /// @return 响应；网络异常（含地址解析失败、超时）返回 std::nullopt
    auto send(std::string_view tag, std::string baseUrl, std::string path, drogon::HttpMethod method, json body,
      std::string bearerToken, f64 timeout, std::optional<u64> sessionId = std::nullopt)
      -> drogon::Task<std::optional<drogon::HttpResponsePtr>>;
} // namespace insoulforge::HttpUtil
