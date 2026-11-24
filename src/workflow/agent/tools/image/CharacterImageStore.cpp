/// @file CharacterImageStore.cpp
/// @brief AI 角色参考图的持久化存储实现

#include <expected>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <workflow/agent/tools/image/CharacterImageStore.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>

namespace insoulforge::CharacterImageStore {
    namespace {
        constexpr std::string_view kImagePath = "data/character-image";
        std::mutex imageMutex;
    } // namespace

    auto load() -> std::expected<std::optional<Image>, std::string> {
        std::lock_guard lock(imageMutex);
        std::ifstream input(std::string(kImagePath), std::ios::binary | std::ios::ate);
        if (!input) {
            std::error_code error;
            const auto status = std::filesystem::status(kImagePath, error);
            if (status.type() == std::filesystem::file_type::not_found) {
                return std::nullopt;
            }
            return std::unexpected("无法打开角色形象图。");
        }
        const auto length = input.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(kMaxImageBytes)) {
            return std::unexpected("角色形象图大小无效。");
        }
        const auto size = static_cast<size_t>(length);
        input.seekg(0);
        std::string bytes(size, '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(size))) {
            return std::unexpected("读取角色形象图失败。");
        }
        const auto mimeType = ImageDescriptionService::staticImageMimeType(bytes);
        if (!mimeType) {
            return std::unexpected("角色形象图格式无效，请重新上传。");
        }
        return Image{.bytes = std::move(bytes), .mimeType = std::string(*mimeType)};
    }

    auto save(const std::string_view bytes) -> std::expected<void, std::string> {
        if (bytes.empty() || bytes.size() > kMaxImageBytes) {
            return std::unexpected("图片不能为空且不得超过 8 MiB。");
        }
        if (!ImageDescriptionService::staticImageMimeType(bytes)) {
            return std::unexpected("仅支持完整的 PNG、JPEG 或 WebP 静态图片。");
        }
        std::lock_guard lock(imageMutex);
        const auto target = std::filesystem::path(kImagePath);
        const auto temporary = target.string() + ".tmp";
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return std::unexpected("无法创建图片存储目录。");
        }
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return std::unexpected("写入角色图片失败。");
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return std::unexpected("替换角色图片失败。");
        }
        return {};
    }

    auto remove() -> bool {
        std::lock_guard lock(imageMutex);
        std::error_code error;
        std::filesystem::remove(std::filesystem::path(kImagePath), error);
        return !error;
    }
} // namespace insoulforge::CharacterImageStore
