/// @file Image.hpp
/// @brief 图片磁盘缓存与按需编码
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <drogon/utils/coroutine.h>
#include <expected>

namespace insoulforge {
    /// @brief 共享缓存图片的轻量句柄；活动句柄保护文件不被清理。
    /// @details 缓存位于工作目录的 cache/images，不属于持久配置。
    ///          ID 为持久化短标识 img_8hex；contentHash() 为完整 SHA-256；无活动句柄的缓存可在三天后或容量超限时淘汰。
    class Image {
    public:
        /// @brief 下载 HTTP/HTTPS 图片并缓存；不跟随重定向，超时 30 秒。
        [[nodiscard]] static auto fromUrl(std::string url) -> drogon::Task<std::expected<Image, std::string>>;
        /// @brief 缓存 PNG、JPEG、WebP 或 GIF 原始数据，上限 24 MiB。
        [[nodiscard]] static auto fromBytes(std::string_view bytes) -> std::expected<Image, std::string>;
        /// @brief 接收标准 Base64、base64:// 或图片 Data URL；实际格式以字节为准。
        [[nodiscard]] static auto fromBase64(std::string_view encoded) -> std::expected<Image, std::string>;
        /// @brief 按短 ID 或旧完整 SHA-256 查找现有缓存，缺失时返回错误，不尝试重新下载。
        [[nodiscard]] static auto find(std::string_view id) -> std::expected<Image, std::string>;
        /// @brief 清理超过指定时间未读取的缓存，并按最久未用优先限制容量，跳过活动句柄。
        [[nodiscard]] static auto cleanup(std::chrono::seconds maxAge = std::chrono::hours(72),
          size_t maxBytes = 1024ULL * 1024 * 1024) -> std::expected<size_t, std::string>;

        [[nodiscard]] static auto mimeType(std::string_view bytes) -> std::optional<std::string_view>;
        [[nodiscard]] static auto hash(std::string_view bytes) -> std::string;
        [[nodiscard]] static auto encode(std::string_view bytes) -> std::string;
        [[nodiscard]] static auto load(std::string source) -> drogon::Task<std::expected<Image, std::string>>;

        [[nodiscard]] auto id() const noexcept -> const std::string & { return m_id; }
        [[nodiscard]] auto contentHash() const noexcept -> const std::string & { return m_contentHash; }
        /// @brief 返回绝对文件位置；不保证文件仍存在，文件只能由本类维护。
        [[nodiscard]] auto path() const -> std::filesystem::path;
        /// @brief 读取原始数据并刷新最后使用时间。
        [[nodiscard]] auto bytes() const -> std::expected<std::string, std::string>;
        /// @brief 按需编码，不在对象中长期保留图片数据。
        [[nodiscard]] auto base64() const -> std::expected<std::string, std::string>;
        /// @brief 返回带实际 MIME 类型的 Base64 Data URL。
        [[nodiscard]] auto dataUrl() const -> std::expected<std::string, std::string>;

    private:
        Image(std::string id, std::string contentHash);
        std::shared_ptr<const std::string> m_handle;
        std::string m_id;
        std::string m_contentHash;
    };
} // namespace insoulforge
