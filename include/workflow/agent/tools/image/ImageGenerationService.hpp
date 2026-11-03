/// @file ImageGenerationService.hpp
/// @brief 文生图请求与 OneBot 图片消息构建

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/image/Image.hpp>

namespace insoulforge::ImageGenerationService {
    struct GeneratedImage {
        std::string message; ///< 用于发送的 OneBot CQ 码
        std::optional<Image> image; ///< 活动缓存句柄
        std::string source; ///< 生成完成后为缓存 ID；解析 HTTP 响应阶段可为原始 URL
    };

    /// @brief 从 OpenAI 兼容的图片响应中提取第一张图片的 CQ 码。
    /// @return URL 或 base64 图片均可用；缺少图片或格式不受支持时返回空。
    [[nodiscard]] auto imageCode(const json &response) -> std::optional<GeneratedImage>;

    /// @brief 按指定尺寸生成或编辑一张图片，按顺序附加所有参考图。
    /// @details 失败时抛出异常供后台任务统一处理。
    auto generate(std::string prompt, std::string size, SessionId sessionId,
      std::vector<std::string> referenceDataUrls = {}) -> drogon::Task<GeneratedImage>;
} // namespace insoulforge::ImageGenerationService
