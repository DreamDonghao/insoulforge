/// @file JevClient.cpp
/// @brief Jev（TypeSafe AI System One）客户端 - 实现

#include <algorithm>
#include <cmath>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/JevClient.hpp>
#include <llm/LlmClient.hpp>

namespace insoulforge::JevClient {
    namespace {
        /// @brief Jev 请求超时（秒），为 LLM 兜底预留时间。
        constexpr f64 kJevTimeoutSeconds = 10.0;

        /// @brief 日志中错误响应体的截断长度
        constexpr size_t kErrorBodyMaxChars = 500;

        [[nodiscard]] auto findAnswer(const json &response, const std::string &questionName) -> const json & {
            return atOrNull(atOrNull(response, "answers"), questionName.c_str());
        }
    } // namespace

    auto isConfigured(const LLMApiConfig &api) -> bool {
        return !api.baseUrl.empty() && !api.path.empty() && !api.model.empty() && !api.apiKey.empty();
    }

    auto requestSystemOne(
      const LLMApiConfig &api, json state, json questions, const u64 sessionId) -> drogon::Task<std::optional<json>> {
        if (!isConfigured(api)) {
            Logger::warn(sessionId, "Jev", "未配置服务地址、请求路径、模型名或密钥，跳过请求");
            co_return std::nullopt;
        }

        json body{{"state", std::move(state)}, {"model", api.model}, {"questions", std::move(questions)}};

        const auto resp =
          co_await HttpUtil::send("Jev", api.baseUrl, api.path, drogon::Post, std::move(body), api.apiKey,
            kJevTimeoutSeconds, sessionId);
        if (!resp) {
            Logger::warn(sessionId, "Jev", "网络异常");
            co_return std::nullopt;
        }

        if ((*resp)->getStatusCode() != drogon::k200OK) {
            const i32 status = static_cast<i32>((*resp)->getStatusCode());
            const std::string respBody = std::string((*resp)->getBody()).substr(0, kErrorBodyMaxChars);
            Logger::error(sessionId, "Jev", fmt::format("请求失败: status={} body={}", status, respBody));
            co_return std::nullopt;
        }

        json parsed;
        if (!tryParseJson((*resp)->body(), parsed)) {
            Logger::error(sessionId, "Jev", "响应 JSON 解析失败");
            co_return std::nullopt;
        }

        const json &answers = atOrNull(parsed, "answers");
        if (!answers.is_object() || answers.empty()) {
            Logger::error(sessionId, "Jev", "响应缺少 answers 或为空对象");
            co_return std::nullopt;
        }

        // Jev 与聊天模型的用量字段不同，只转换记账所需的字段。
        const i32 inputTokens = getInt(atOrNull(parsed, "usage"), "input_tokens");
        const i32 outputTokens = getInt(atOrNull(parsed, "usage"), "output_tokens");
        const json usageForAccounting = {{"usage", {{"prompt_tokens", inputTokens}, {"completion_tokens", outputTokens},
                                                     {"total_tokens", inputTokens + outputTokens}}}};
        LlmClient::logUsage(usageForAccounting, getStr(parsed, "model", api.model), "jev", sessionId);

        co_return parsed;
    }

    auto readChoice(
      const json &response, const std::string &questionName, const std::vector<std::string_view> &validLabels)
      -> std::optional<std::string> {
        const json &answer = findAnswer(response, questionName);
        const json &choiceValue = atOrNull(answer, "choice");
        if (!choiceValue.is_string()) {
            return std::nullopt;
        }
        const std::string label = choiceValue.get<std::string>();
        const bool isValid = std::ranges::any_of(validLabels, [&label](const std::string_view valid) {
            return valid == label;
        });
        if (!isValid) {
            return std::nullopt;
        }
        return label;
    }

    auto readConfidence(const json &response, const std::string &questionName) -> std::optional<double> {
        const json &answer = findAnswer(response, questionName);
        const json &confidenceValue = atOrNull(answer, "confidence");
        if (!confidenceValue.is_number()) {
            return std::nullopt;
        }
        const f64 confidence = confidenceValue.get<double>();
        if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
            return std::nullopt;
        }
        return confidence;
    }
} // namespace insoulforge::JevClient
