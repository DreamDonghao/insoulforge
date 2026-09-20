/// @file LlmClient.hpp
/// @brief API 客户端 - LLM API 请求封装
/// @date 2026-04-02
/// @details 封装 LLM API 请求与用量统计：
///          - Chat 请求体构建与响应校验：buildChatRequestBody() / validChatJson()
///          - Chat completion 请求（带重试）：requestChat()
///          - LLM 请求：requestLLM()
///          - Embedding 请求：requestEmbedding()
///          - 缓存命中率日志：logUsage()

#pragma once
#include <drogon/utils/coroutine.h>
#include <infrastructure/JsonUtil.hpp>
#include <optional>
#include <string>
#include <vector>

namespace insoulforge {
    struct LLMApiConfig;
    struct LLMModelParams;
} // namespace insoulforge

/// @brief API 客户端 - 封装 LLM API 请求与用量统计
namespace insoulforge::LlmClient {
    /// @brief 判断 API 配置是否具备发起请求所需的地址、路径和模型名。
    [[nodiscard]] bool isConfigured(const LLMApiConfig &api);

    /// @brief 构建 OpenAI 兼容 chat 请求体（model/messages/采样参数；reasoningEffort 非空才附带）
    /// @param tools 工具定义；非 null 时附带 tools 字段
    json buildChatRequestBody(const LLMApiConfig &api, const LLMModelParams &params, json messages, json tools = {});

    /// @brief 校验 chat 响应：200 + JSON body + choices 为非空数组
    /// @return 合法时返回完整响应 JSON；否则 nullopt（错误日志由调用方按上下文输出）
    std::optional<json> validChatJson(const drogon::HttpResponsePtr &resp);

    /// @brief 请求 chat completion（网络异常或临时性 HTTP 错误 503/429/500 时重试）
    /// @param label 日志与请求标签（如 "LLM" / "深度思考"）
    /// @param usageRole 用量记录中的角色标识（executor / executorThinking / memory 等）
    /// @param apiConfig LLM API 配置
    /// @param params 采样参数
    /// @param messages 消息列表
    /// @param tools 工具定义；null 表示不附带
    /// @param sessionId 会话 ID
    /// @return 校验通过的完整响应 JSON；不可恢复错误或重试耗尽时返回 std::nullopt
    drogon::Task<std::optional<json>> requestChat(std::string label, std::string usageRole,
      const LLMApiConfig &apiConfig, const LLMModelParams &params, json messages, json tools = {},
      uint64_t sessionId = 0);

    /// @brief 请求 LLM API（使用 Executor 配置）
    /// @param messages 消息列表
    /// @param temperature 温度参数
    /// @param top_p Top-P 采样参数
    /// @param max_tokens 最大 token 数
    /// @param role
    /// @param sessionId
    /// @param timeoutSeconds 请求超时秒数；后台维护任务默认 180 秒
    /// @return 响应文本，失败返回 std::nullopt
    drogon::Task<std::optional<std::string>> requestLLM(json messages, double temperature = 1.35, double top_p = 0.92,
      int max_tokens = 1024, std::string role = "memory", std::optional<uint64_t> sessionId = std::nullopt,
      double timeoutSeconds = 180.0);

    /// @brief 从 API 响应中提取 usage 信息并输出缓存命中率日志
    /// @param responseJson API 返回的完整 JSON
    /// @param model 模型名
    /// @param role 角色名（router/executor/executorThinking/memory/image/embedding）
    /// @param sessionId
    void logUsage(const json &responseJson, const std::string &model, const std::string &role,
      std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 请求 Embedding API（使用 Embedding 配置）
    /// @param text 待向量化的文本
    /// @param sessionId
    /// @return 向量，未配置或失败返回 std::nullopt
    drogon::Task<std::optional<std::vector<float>>> requestEmbedding(
      std::string text, std::optional<uint64_t> sessionId = std::nullopt);
} // namespace insoulforge::LlmClient
