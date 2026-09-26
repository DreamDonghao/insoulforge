/// @file ImageDescriptionStore.cpp
/// @brief 图片视觉描述缓存存储实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <media/ImageDescriptionStore.hpp>

namespace insoulforge::ImageDescriptionStore {
    namespace {
        /// @brief 删除超过十天未命中的缓存；调用方必须持有数据库写锁
        size_t purgeExpiredLocked(sqlite3 *database) {
            const Statement stmt(
              database, "DELETE FROM image_description_cache WHERE updated_at < datetime('now', '-10 days')");
            stmt.exec();
            return static_cast<size_t>(sqlite3_changes(database));
        }
    } // namespace

    std::optional<CachedImageDescription> find(
      const std::string &contentHash, const std::string &model, const i32 promptVersion) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        purgeExpiredLocked(db.handle());

        std::optional<CachedImageDescription> cached;
        {
            const Statement stmt(db.handle(),
              "SELECT status, description, sampled_frame_count FROM image_description_cache WHERE content_hash = ? "
              "AND model = ? AND prompt_version = ? AND "
              "((status = 'succeeded' AND updated_at >= datetime('now', '-10 days')) "
              "OR (status = 'failed' AND updated_at >= datetime('now', '-10 minutes')))");
            stmt.bind(1, contentHash);
            stmt.bind(2, model);
            stmt.bind(3, promptVersion);
            if (stmt.step()) {
                cached = CachedImageDescription{.succeeded = stmt.getText(0) == "succeeded",
                  .description = stmt.getText(1),
                  .sampledFrameCount = stmt.getInt(2)};
            }
        }
        if (!cached) {
            return std::nullopt;
        }

        const Statement touch(db.handle(), "UPDATE image_description_cache SET updated_at = CURRENT_TIMESTAMP "
                                           "WHERE content_hash = ? AND model = ? AND prompt_version = ?");
        touch.bind(1, contentHash);
        touch.bind(2, model);
        touch.bind(3, promptVersion);
        touch.exec();
        return cached;
    }

    void upsert(const std::string &contentHash, const std::string &model, const i32 promptVersion,
      const std::string &mediaType, const bool succeeded, const std::string &description, const i32 sampledFrameCount) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        purgeExpiredLocked(db.handle());
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

    size_t purgeExpired() {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        return purgeExpiredLocked(db.handle());
    }

    size_t clearAll() {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM image_description_cache");
        stmt.exec();
        return static_cast<size_t>(sqlite3_changes(db.handle()));
    }
} // namespace insoulforge::ImageDescriptionStore
