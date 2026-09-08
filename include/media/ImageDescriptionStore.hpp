/// @file ImageDescriptionStore.hpp
/// @brief 图片视觉描述缓存存储

#pragma once
#include <optional>
#include <string>

namespace insoulforge {
    /// @brief 已缓存的图片视觉描述
    struct CachedImageDescription {
        bool succeeded; ///< 视觉模型是否成功生成描述
        std::string description; ///< 成功时的图片或动图描述
        int sampledFrameCount; ///< 生成描述时送入视觉模型的帧数
    };

    namespace ImageDescriptionStore {
        /// @brief 查询与当前视觉模型及提示词匹配的缓存
        /// @details 成功结果长期复用；失败结果仅在十分钟内复用，避免持续重试失效媒体。
        [[nodiscard]] std::optional<CachedImageDescription> find(
          const std::string &contentHash, const std::string &model, int promptVersion);

        /// @brief 写入或更新一条视觉描述缓存
        void upsert(const std::string &contentHash, const std::string &model, int promptVersion,
          const std::string &mediaType, bool succeeded, const std::string &description, int sampledFrameCount);

        /// @brief 清除全部图片视觉描述缓存
        /// @return 已删除的缓存条数
        [[nodiscard]] size_t clearAll();
    } // namespace ImageDescriptionStore
} // namespace insoulforge
