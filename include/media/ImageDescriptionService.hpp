/// @file ImageDescriptionService.hpp
/// @brief 图片与动图的视觉描述服务

#pragma once
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

namespace insoulforge {
    /// @brief 图片视觉描述结果
    struct ImageDescriptionResult {
        std::string contentHash; ///< 原始媒体字节的 SHA-256
        std::string mediaType; ///< image 或 gif
        std::string description; ///< 视觉模型生成的语义描述
        int sampledFrameCount{1}; ///< 发送给视觉模型的帧数
    };

    namespace ImageDescriptionService {
        /// @brief 下载媒体、查询缓存并生成视觉描述
        /// @param sourceUrl OneBot 提供的媒体 URL
        /// @param sessionId 关联日志与用量的会话 ID
        /// @return 成功时返回描述；下载、解码或模型调用失败时返回空值
        /// @details GIF 最多向视觉模型提交 16 帧；超过时按播放时间均匀抽样并保留首尾帧。
        [[nodiscard]] drogon::Task<std::optional<ImageDescriptionResult>> describe(
          std::string sourceUrl, uint64_t sessionId);
    } // namespace ImageDescriptionService
} // namespace insoulforge
