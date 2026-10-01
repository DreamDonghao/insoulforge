/// @file JevClient.hpp
/// @brief Jev 决策接口的 HTTP 请求与响应解析
/// @details 路由规则和问题内容由 MessageRouter 负责；此处只处理传输、用量与答案字段。

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
    [[nodiscard]] auto isConfigured(const LLMApiConfig &api) -> bool;

    /// @brief 请求一次 System One 判定
    /// @param api Jev API 配置（地址、路径、模型、密钥）
    /// @param state 被评估的程序状态；按值接管，由协程帧持有
    /// @param questions 问题映射，须非空；按值接管，由协程帧持有
    /// @param sessionId 会话 ID，仅用于日志归属
    /// @return 校验通过的完整响应 JSON（含 answers/model/usage）；未配置、网络异常、
    ///         非 200、JSON 非法或 answers 缺失/为空时返回 std::nullopt
    /// @details 超时 10 秒；失败时由 Router 转交原有 LLM 判断，不在此处重试。
    [[nodiscard]] auto requestSystemOne(
      const LLMApiConfig &api, json state, json questions, u64 sessionId = 0) -> drogon::Task<std::optional<json>>;

    /// @brief 从响应中读取 choice 类型答案的胜出 label
    /// @param response requestSystemOne 返回的响应 JSON
    /// @param questionName 问题名
    /// @param validLabels 合法 label 集合
    /// @return 胜出 label；字段缺失、非字符串或 label 不属于 validLabels 时返回 std::nullopt
    [[nodiscard]] auto readChoice(
      const json &response, const std::string &questionName, const std::vector<std::string_view> &validLabels)
      -> std::optional<std::string>;

    /// @brief 从响应中读取 choice 类型答案的 confidence
    /// @param response requestSystemOne 返回的响应 JSON
    /// @param questionName 问题名
    /// @return confidence（0.0~1.0）；答案缺失、类型不符或越界时返回 std::nullopt
    /// @details Router 用此值决定是否采纳 Jev 结果。
    [[nodiscard]] auto readConfidence(const json &response, const std::string &questionName)
      -> std::optional<double>;
} // namespace insoulforge::JevClient
