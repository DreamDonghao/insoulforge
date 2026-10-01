/// @file JevClient.hpp
/// @brief Jev（TypeSafe AI System One）客户端 - OpenRouter decisions 请求封装
/// @details 职责边界限定在 HTTP 传输与契约解析：不含路由策略，问题定义、
///          label 语义、state 构建归 MessageRouter。
///          不复用 LlmClient：Jev 响应根对象只有 answers/model/usage，与
///          LlmClient::validChatJson() 要求的非空 choices 数组不同；请求体为
///          state/questions，与 LlmClient::buildChatRequestBody() 产出的
///          messages 加采样参数形态不同。

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    struct LLMApiConfig;
} // namespace insoulforge

namespace insoulforge::JevClient {
    /// @brief 判断配置是否具备发起 Jev 请求所需的地址、路径、模型名和密钥
    /// @details 相比 LlmClient::isConfigured 额外校验 apiKey。默认配置下地址、路径、
    ///          模型名都非空，缺密钥的请求会返回 401。
    [[nodiscard]] auto isConfigured(const LLMApiConfig &api) -> bool;

    /// @brief 请求一次 System One 判定
    /// @param api Jev API 配置（地址、路径、模型、密钥）
    /// @param state 被评估的程序状态；按值接管，由协程帧持有
    /// @param questions 问题映射，须非空；按值接管，由协程帧持有
    /// @param sessionId 会话 ID，仅用于日志归属
    /// @return 校验通过的完整响应 JSON（含 answers/model/usage）；未配置、网络异常、
    ///         非 200、JSON 非法或 answers 缺失/为空时返回 std::nullopt
    /// @details 超时取 10s 量级，短于 LlmClient 的 90s，为优先路径失败后的兜底留出延迟预算。
    ///          不实现重试，兜底路径本身承担重试职责。
    [[nodiscard]] auto requestSystemOne(
      const LLMApiConfig &api, json state, json questions, u64 sessionId = 0) -> drogon::Task<std::optional<json>>;

    /// @brief 从响应中读取 choice 类型答案的胜出 label
    /// @param response requestSystemOne 返回的响应 JSON
    /// @param questionName 问题名
    /// @param validLabels 合法 label 集合
    /// @return 胜出 label；答案缺失、类型不符、或 label 不属于 validLabels 时返回 std::nullopt
    /// @details 独立导出以支持无网络的离线测试。label 语义（如 skip/reply/unclear 的
    ///          含义）属于路由策略，不在本模块判定；本函数把胜出 label 原样读出。
    [[nodiscard]] auto readChoice(
      const json &response, const std::string &questionName, const std::vector<std::string_view> &validLabels)
      -> std::optional<std::string>;

    /// @brief 从响应中读取 choice 类型答案的 confidence
    /// @param response requestSystemOne 返回的响应 JSON
    /// @param questionName 问题名
    /// @return confidence（0.0~1.0）；答案缺失、类型不符或越界时返回 std::nullopt
    /// @details confidence 不参与判定，仅供调用方日志与后续 hybrid 升级的观测数据。
    ///          独立导出以支持无网络的离线测试。
    [[nodiscard]] auto readConfidence(const json &response, const std::string &questionName)
      -> std::optional<double>;
} // namespace insoulforge::JevClient
