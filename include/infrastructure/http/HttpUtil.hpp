/// @file HttpUtil.hpp
/// @brief 出站 HTTP 请求与调试记录
/// @details send() 将请求与响应写入内存调试记录。常规运行日志只记录错误或异常，
///          不记录 Authorization 头；图片数据会被替换为占位文本。

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <drogon/utils/coroutine.h>
#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::HttpUtil {
    /// @brief 复制 JSON 并省略其中的 base64 图片，保留提示词和其他可读参数。
    [[nodiscard]] auto redactImagePayloads(const json &value) -> json;

    /// @brief 发送 HTTP 请求，记录请求内容与异常
    /// @param tag 日志来源和调试记录标签，如 Router、Executor
    /// @param baseUrl 服务 Base URL，可包含路径前缀，如 https://api.example.com/v1
    /// @param path 请求路径，如 /chat/completions；会与 Base URL 中的路径前缀合并
    /// @param method HTTP 方法
    /// @param body JSON 请求体（null 表示无 body，例如 GET；按值接管，协程帧持有）
    /// @param bearerToken Bearer 认证 token（空串则不添加 Authorization 头）
    /// @param timeout 超时秒数
    /// @param sessionId 关联的会话 ID；缺省时视为全局请求
    /// @param traceResponse 是否保留原始响应体；为 false 时只记录脱敏后的 JSON 响应
    /// @return 成功时为 HTTP 响应（含 4xx/5xx），建连或发送失败时为错误说明
    auto send(std::string_view tag, std::string baseUrl, std::string path, drogon::HttpMethod method, json body,
      std::string bearerToken, f64 timeout, std::optional<u64> sessionId = std::nullopt, bool traceResponse = true)
      -> drogon::Task<std::expected<drogon::HttpResponsePtr, std::string>>;
} // namespace insoulforge::HttpUtil
