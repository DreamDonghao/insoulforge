/// @file ImageDescriptionStore.cpp
/// @brief 图片视觉描述缓存存储实现

#include <infrastructure/storage/Database.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::ImageDescriptionStore {
    std::optional<CachedImageDescription> find(
      const std::string &contentHash, const std::string &model, const int promptVersion) {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "SELECT status, description, sampled_frame_count FROM image_description_cache WHERE content_hash = ? AND "
          "model = ? "
          "AND prompt_version = ? AND (status = 'succeeded' OR updated_at >= datetime('now', '-10 minutes'))");
        stmt.bind(1, contentHash);
        stmt.bind(2, model);
        stmt.bind(3, promptVersion);
        if (!stmt.step())
            return std::nullopt;
        return CachedImageDescription{.succeeded = stmt.getText(0) == "succeeded",
          .description = stmt.getText(1),
          .sampledFrameCount = stmt.getInt(2)};
    }

    void upsert(const std::string &contentHash, const std::string &model, const int promptVersion,
      const std::string &mediaType, const bool succeeded, const std::string &description, const int sampledFrameCount) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO image_description_cache (content_hash, model, prompt_version, media_type, status, description, "
          "sampled_frame_count) VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT(content_hash, model, prompt_version) "
          "DO UPDATE SET media_type = excluded.media_type, status = excluded.status, "
          "description = excluded.description, sampled_frame_count = excluded.sampled_frame_count, "
          "updated_at = CURRENT_TIMESTAMP");
        stmt.bind(1, contentHash);
        stmt.bind(2, model);
        stmt.bind(3, promptVersion);
        stmt.bind(4, mediaType);
        stmt.bind(5, succeeded ? "succeeded" : "failed");
        stmt.bind(6, description);
        stmt.bind(7, sampledFrameCount);
        stmt.exec();
    }

    size_t clearAll() {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM image_description_cache");
        stmt.exec();
        return static_cast<size_t>(sqlite3_changes(db.handle()));
    }
} // namespace insoulforge::ImageDescriptionStore
