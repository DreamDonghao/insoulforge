/// @file ImageGenerationService.hpp
/// @brief 文生图请求与 OneBot 图片消息构建

#pragma once

#include <optional>
#include <string>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::ImageGenerationService {
    struct GeneratedImage {
        std::string message; ///< 用于发送的 OneBot CQ 码
        std::string source; ///< 用于视觉识别的原始 URL 或 base64:// 数据
    };

    /// @brief 从 OpenAI 兼容的图片响应中提取第一张图片的 CQ 码。
    /// @return URL 或 base64 图片均可用；缺少图片或格式不受支持时返回空。
    [[nodiscard]] auto imageCode(const json &response) -> std::optional<GeneratedImage>;

    /// @brief 按指定尺寸生成或编辑一张图片；referenceDataUrl 存在时用作原图。
    /// @details 失败时抛出异常供后台任务统一处理。
    auto generate(std::string prompt, std::string size, u64 sessionId,
      std::optional<std::string> referenceDataUrl = std::nullopt) -> drogon::Task<GeneratedImage>;
} // namespace insoulforge::ImageGenerationService
