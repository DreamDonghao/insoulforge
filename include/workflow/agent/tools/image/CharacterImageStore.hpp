/// @file CharacterImageStore.hpp
/// @brief AI 角色参考图的持久化存储

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace insoulforge::CharacterImageStore {
    constexpr size_t kMaxImageBytes = 8U * 1024U * 1024U;

    struct Image {
        std::string bytes;
        std::string mimeType;
    };

    /// @brief 读取当前角色图；仅文件不存在时返回成功的空值。
    [[nodiscard]] auto load() -> std::expected<std::optional<Image>, std::string>;

    /// @brief 校验并原子替换角色图；失败时返回用户可读的原因。
    [[nodiscard]] auto save(std::string_view bytes) -> std::expected<void, std::string>;

    /// @brief 删除当前角色图；原本不存在也视为成功。
    [[nodiscard]] auto remove() -> bool;
} // namespace insoulforge::CharacterImageStore
