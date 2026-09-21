/// @file LlmClient.cpp
/// @brief API 客户端 - 实现


#include <admin/realtime/WebSocketManager.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
#include <llm/UsageStore.hpp>

namespace insoulforge {
    namespace {
        /// @brief 网络异常/临时性 HTTP 错误的最大重试次数
        constexpr int kMaxRetries = 3;

        /// @brief 重试间隔
        constexpr std::chrono::seconds kRetryDelay{1};

        /// @brief LLM 请求超时（秒）
        constexpr double kLlmTimeoutSeconds = 90.0;

        /// @brief 日志中错误响应体的截断长度
        constexpr size_t kErrorBodyMaxChars = 500;

        /// @brief 是否为可重试的临时性 HTTP 状态码
        bool isRetryableStatus(const int status) { return status == 503 || status == 429 || status == 500; }

        /// @brief 通用 API 请求函数
        drogon::Task<std::optional<std::string>> requestStr(json messages, std::string base_url, std::string path,
          std::string api_key, std::string model, const double temperature, const double top_p, const int max_tokens,
          std::string role, const std::optional<uint64_t> sessionId, const double timeoutSeconds) {
            const LLMApiConfig api{.apiKey = api_key, .baseUrl = base_url, .path = path, .model = model};
            if (!LlmClient::isConfigured(api)) {
                if (sessionId) {
                    Logger::warn(*sessionId, "LLM", fmt::format("未配置服务地址、请求路径或模型名，跳过请求"));
                } else {
                    Logger::warn(0, "LLM", fmt::format("未配置服务地址、请求路径或模型名，跳过请求"));
                }
                co_return std::nullopt;
            }
            const LLMModelParams params{.maxTokens = max_tokens, .temperature = temperature, .topP = top_p};
            json body = LlmClient::buildChatRequestBody(api, params, std::move(messages));
            const auto resp = co_await HttpUtil::send("[LLM]", std::move(base_url), std::move(path), drogon::Post,
              std::move(body), std::move(api_key), timeoutSeconds, sessionId);
            if (!resp) {
                co_return std::nullopt;
            }

            const auto respJson = LlmClient::validChatJson(*resp);
            if (!respJson) {
                if (sessionId) {
                    Logger::error(*sessionId, "LLM",
                      fmt::format("请求出错: status={}", static_cast<int>((*resp)->getStatusCode())));
                } else {
                    Logger::error(
                      0, "LLM", fmt::format("请求出错: status={}", static_cast<int>((*resp)->getStatusCode())));
                }
                co_return std::nullopt;
            }

            LlmClient::logUsage(*respJson, model, role, sessionId);

            // validChatJson 已校验 choices 为非空数组
            const json &message = atOrNull((*respJson)["choices"][0], "message");
            co_return jsonToString(atOrNull(message, "content"));
        }
    } // namespace

    namespace LlmClient {
        bool isConfigured(const LLMApiConfig &api) {
            return !api.baseUrl.empty() && !api.path.empty() && !api.model.empty();
        }

        json buildChatRequestBody(const LLMApiConfig &api, const LLMModelParams &params, json messages, json tools) {
            json body;
            body["model"] = api.model;
            body["messages"] = std::move(messages);
            if (!tools.is_null())
                body["tools"] = std::move(tools);
            body["temperature"] = params.temperature;
            body["max_tokens"] = params.maxTokens;
            body["top_p"] = params.topP;
            if (!api.reasoningEffort.empty())
                body["reasoning_effort"] = api.reasoningEffort;
            return body;
        }

        std::optional<json> validChatJson(const drogon::HttpResponsePtr &resp) {
            if (!resp || resp->getStatusCode() != drogon::k200OK)
                return std::nullopt;
            json parsed;
            if (!tryParseJson(resp->body(), parsed))
                return std::nullopt;
            // choices 必须为非空数组（调用方直接按 [choices][0] 取 message）
            const json &choices = atOrNull(parsed, "choices");
            if (!choices.is_array() || choices.empty())
                return std::nullopt;
            return parsed;
        }

        drogon::Task<std::optional<json>> requestChat(std::string label, std::string usageRole,
          const LLMApiConfig &apiConfig, const LLMModelParams &params, json messages, json tools,
          const uint64_t sessionId) {
            const std::string tag = "[" + label + "]";
            if (!isConfigured(apiConfig)) {
                Logger::warn(sessionId, "LLM", fmt::format("{} 未配置服务地址、请求路径或模型名，跳过请求", tag));
                co_return std::nullopt;
            }
            Logger::debug(sessionId, "LLM", fmt::format("{} model={}", tag, apiConfig.model));

            const json body = buildChatRequestBody(apiConfig, params, std::move(messages), std::move(tools));

            for (int attempt = 0;; ++attempt) {
                const auto resp = co_await HttpUtil::send(tag, apiConfig.baseUrl, apiConfig.path, drogon::Post, body,
                  apiConfig.apiKey, kLlmTimeoutSeconds, sessionId);

                if (!resp) {
                    Logger::warn(sessionId, "LLM", fmt::format("{}网络异常", tag));
                } else if (const auto respJson = validChatJson(*resp)) {
                    logUsage(*respJson, apiConfig.model, usageRole, sessionId);
                    co_return respJson;
                } else {
                    const int status = static_cast<int>((*resp)->getStatusCode());
                    const std::string respBody = std::string((*resp)->getBody()).substr(0, kErrorBodyMaxChars);
                    Logger::error(sessionId, "LLM", fmt::format("{}失败: status={} body={}", tag, status, respBody));
                    if (!isRetryableStatus(status)) {
                        co_return std::nullopt; // 不可恢复的错误（鉴权、参数等）
                    }
                    Logger::warn(sessionId, "LLM", fmt::format("{}临时性错误", tag));
                }

                if (attempt >= kMaxRetries) {
                    co_return std::nullopt; // 重试耗尽
                }
                Logger::warn(sessionId, "LLM", fmt::format("{}第 {}/{} 次重试", tag, attempt + 1, kMaxRetries));
                co_await drogon::sleepCoro(drogon::app().getLoop(), kRetryDelay);
            }
        }
    } // namespace LlmClient

    drogon::Task<std::optional<std::string>> LlmClient::requestLLM(json messages, const double temperature,
      const double top_p, const int max_tokens, std::string role, const std::optional<uint64_t> sessionId,
      const double timeoutSeconds) {
        const auto &config = Config::instance();
        co_return co_await requestStr(std::move(messages), config.executor.baseUrl, config.executor.path,
          config.executor.apiKey, config.executor.model, temperature, top_p, max_tokens, std::move(role), sessionId,
          timeoutSeconds);
    }

    drogon::Task<std::optional<std::vector<float>>> LlmClient::requestEmbedding(
      std::string text, const std::optional<uint64_t> sessionId) {
        const auto &config = Config::instance().embedding;
        if (!isConfigured(config)) {
            Logger::debug(0, "LLM", fmt::format("Embedding 未配置，跳过向量化"));
            co_return std::nullopt;
        }

        json body;
        body["model"] = config.model;
        body["input"].push_back(std::move(text));

        const auto resp = co_await HttpUtil::send(
          "[Embedding]", config.baseUrl, config.path, drogon::Post, std::move(body), config.apiKey, 30.0, sessionId);
        if (!resp) {
            co_return std::nullopt;
        }

        json respJson;
        const bool requestOk = (*resp)->getStatusCode() == drogon::k200OK && tryParseJson((*resp)->body(), respJson);
        const json &data = atOrNull(respJson, "data");
        if (!requestOk || !data.is_array() || data.empty()) {
            if (sessionId) {
                Logger::error(*sessionId, "Embedding",
                  fmt::format("请求出错: status={}", static_cast<int>((*resp)->getStatusCode())));
            } else {
                Logger::error(
                  0, "Embedding", fmt::format("请求出错: status={}", static_cast<int>((*resp)->getStatusCode())));
            }
            co_return std::nullopt;
        }

        LlmClient::logUsage(respJson, config.model, "embedding", sessionId);

        std::vector<float> embedding;
        for (const auto &value: atOrNull(data[0], "embedding")) {
            embedding.push_back(static_cast<float>(jsonToDouble(value)));
        }
        if (embedding.empty()) {
            co_return std::nullopt;
        }
        co_return embedding;
    }

    void LlmClient::logUsage(const json &responseJson, const std::string &model, const std::string &role,
      const std::optional<uint64_t> sessionId) {
        if (!responseJson.contains("usage"))
            return;
        const json &usage = responseJson["usage"];
        int promptTokens = getInt(usage, "prompt_tokens");
        int completionTokens = getInt(usage, "completion_tokens");
        int totalTokens = getInt(usage, "total_tokens");

        int cachedTokens = 0;
        // OpenAI-compatible: prompt_tokens_details.cached_tokens（0 命中也是有效数据，不能当作缺失）
        if (usage.contains("prompt_tokens_details")) {
            const json &details = usage["prompt_tokens_details"];
            cachedTokens = getInt(details, "cached_tokens");
        } else {
            // 部分网关用 prompt_cache_hit_tokens / prompt_cache_miss_tokens 表达
            const int hitTokens = getInt(usage, "prompt_cache_hit_tokens");
            const int missTokens = getInt(usage, "prompt_cache_miss_tokens");
            cachedTokens = hitTokens;
            if (promptTokens == 0) {
                promptTokens = hitTokens + missTokens;
            }
        }

        const uint64_t logSessionId = sessionId.value_or(0);
        if (promptTokens > 0) {
            float hitRate = static_cast<float>(cachedTokens) / static_cast<float>(promptTokens) * 100.0f;
            Logger::info(logSessionId, "Usage",
              fmt::format("role={} | model={} | prompt={} | completion={} | total={} | cached={} | hit_rate={:.1f}%",
                role, model, promptTokens, completionTokens, totalTokens, cachedTokens, hitRate));
        } else if (totalTokens > 0) {
            // 网关偶尔不返回 prompt 分解，用 total - completion 兜底，避免用量统计缺 prompt 数据
            promptTokens = std::max(0, totalTokens - completionTokens);
            const auto usageText = dumpJson(usage, false);
            Logger::info(logSessionId, "Usage",
              fmt::format("role={} | model={} | prompt={} (no breakdown) | completion={} | total={} | cached=N/A | "
                          "hit_rate=N/A | usage={}",
                role, model, promptTokens, completionTokens, totalTokens, usageText));
        }

        UsageStore::addUsageRecord(role, model, promptTokens, completionTokens, totalTokens, cachedTokens);

        json evt;
        evt["role"] = role;
        evt["model"] = model;
        if (sessionId.has_value()) {
            evt["groupId"] = *sessionId;
        }
        WebSocketManager::instance().broadcastEvent("usage_updated", evt);
    }
} // namespace insoulforge
