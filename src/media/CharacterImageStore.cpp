/// @file CharacterImageStore.cpp
/// @brief AI 角色参考图的持久化存储实现

#include <filesystem>
#include <fstream>
#include <mutex>

#include <media/CharacterImageStore.hpp>
#include <media/ImageDescriptionService.hpp>

namespace insoulforge::CharacterImageStore {
    namespace {
        constexpr std::string_view kImagePath = "data/character-image";
        std::mutex imageMutex;
    } // namespace

    auto load() -> std::optional<Image> {
        std::lock_guard lock(imageMutex);
        std::ifstream input(std::string(kImagePath), std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0 || input.tellg() > static_cast<std::streamoff>(kMaxImageBytes)) {
            return std::nullopt;
        }
        const auto size = static_cast<size_t>(input.tellg());
        input.seekg(0);
        std::string bytes(size, '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(size))) {
            return std::nullopt;
        }
        const auto mimeType = ImageDescriptionService::staticImageMimeType(bytes);
        return mimeType ? std::optional<Image>{Image{std::move(bytes), std::string(*mimeType)}} : std::nullopt;
    }

    auto save(const std::string_view bytes) -> std::optional<std::string> {
        if (bytes.empty() || bytes.size() > kMaxImageBytes) {
            return "图片不能为空且不得超过 8 MiB。";
        }
        if (!ImageDescriptionService::staticImageMimeType(bytes)) {
            return "仅支持完整的 PNG、JPEG 或 WebP 静态图片。";
        }
        std::lock_guard lock(imageMutex);
        const auto target = std::filesystem::path(kImagePath);
        const auto temporary = target.string() + ".tmp";
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return "无法创建图片存储目录。";
        }
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return "写入角色图片失败。";
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return "替换角色图片失败。";
        }
        return std::nullopt;
    }

    auto remove() -> bool {
        std::lock_guard lock(imageMutex);
        std::error_code error;
        std::filesystem::remove(std::filesystem::path(kImagePath), error);
        return !error;
    }
} // namespace insoulforge::CharacterImageStore
