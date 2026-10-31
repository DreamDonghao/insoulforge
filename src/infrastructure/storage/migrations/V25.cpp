/// @file V25.cpp
/// @brief 将历史记录一次性转换为当前格式；调用方负责事务和版本号。
#include <algorithm>
#include <unordered_set>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V25.hpp>

namespace insoulforge::SchemaMigrator {
    namespace {
        void renameUserId(json &identity) {
            if (!identity.is_object() || !identity.contains("qq"))
                return;
            if (!identity.contains("user_id"))
                identity["user_id"] = identity["qq"];
            identity.erase("qq");
        }

        auto migrateSegment(json segment) -> json {
            renameUserId(segment);
            const auto type = getStr(segment, "type");
            if ((type == "at" || type == "poke") && !atOrNull(segment, "target").is_object()) {
                const auto userId = type == "at" ? getStr(segment, "user_id") : getStr(segment, "target_id");
                segment["target"] = type == "at" && (userId == "0" || userId == "all")
                  ? json{{"kind", "all"}} : json{{"user_id", userId}};
                const auto name = getStr(segment, type == "at" ? "name" : "target_name");
                if (!name.empty() && name != "未知")
                    segment["target"]["name"] = name;
                for (const auto *key: {"user_id", "name", "target_id", "target_name"})
                    segment.erase(key);
            }
            if (type == "notification") {
                segment["type"] = "member_event";
                auto action = getStr(segment, "action", getStr(segment, "kind"));
                if (action.starts_with("member_"))
                    action.erase(0, std::string_view("member_").size());
                segment["action"] = action;
                const auto reason = getStr(segment, "reason", getStr(segment, "sub_type"));
                if (!reason.empty())
                    segment["reason"] = reason;
                if (const auto operatorId = getUInt(segment, "operator_id"); operatorId > 0)
                    segment["operator"] = {{"user_id", std::to_string(operatorId)}};
                for (const auto *key: {"kind", "sub_type", "operator_id"})
                    segment.erase(key);
            }
            for (const auto *key: {"target", "operator"})
                if (segment.contains(key))
                    renameUserId(segment[key]);
            if (type == "at") {
                const auto userId = getStr(atOrNull(segment, "target"), "user_id");
                if (userId == "0" || userId == "all")
                    segment["target"] = {{"kind", "all"}};
            }
            return segment;
        }

        void normalizeImageSource(json &image) {
            if (!image.is_object())
                throw DbError("历史图片资产格式无效，无法迁移");
            if (!atOrNull(image, "source").is_object() && (image.contains("file") || image.contains("url")))
                image["source"] = {{"file", getStr(image, "file")}, {"url", getStr(image, "url")}};
            image.erase("file");
            image.erase("url");
        }

        auto migrateRecord(const std::string &content, const std::string &role) -> json {
            json record;
            if (!tryParseJson(content, record) || !record.is_object())
                record = {{"text", content}};
            if (record.contains("sender"))
                renameUserId(record["sender"]);
            else if (role == "assistant")
                record["sender"] = {{"user_id", "self"}};

            auto images = atOrNull(atOrNull(record, "assets"), "images");
            if (!images.is_array())
                images = atOrNull(record, "images");
            if (!images.is_array())
                images = json::array();
            for (auto &image: images)
                normalizeImageSource(image);

            auto segments = atOrNull(record, "segments");
            if (!segments.is_array() || segments.empty()) {
                segments = json::array();
                if (const auto text = getStr(record, "text"); !text.empty())
                    segments.push_back({{"type", "text"}, {"text", text}});
                for (size_t i = 0; i < images.size(); ++i)
                    segments.push_back({{"type", "image"}, {"image_index", i}});
                for (const auto &face: atOrNull(record, "faces")) {
                    auto segment = face;
                    segment["type"] = "face";
                    segments.push_back(std::move(segment));
                }
                for (auto notification: atOrNull(record, "notifications")) {
                    if (!notification.is_object())
                        throw DbError("历史成员事件格式无效，无法迁移");
                    if (!notification.contains("type"))
                        notification["type"] = "notification";
                    segments.push_back(std::move(notification));
                }
            }
            size_t nextImage = 0;
            for (auto &segment: segments) {
                if (!segment.is_object())
                    throw DbError("历史消息段格式无效，无法迁移");
                segment = migrateSegment(std::move(segment));
                if (getStr(segment, "type") != "image")
                    continue;
                const auto index = getInt64(segment, "image_index", static_cast<i64>(nextImage));
                if (index < 0)
                    throw DbError("历史图片索引无效，无法迁移");
                segment["image_index"] = index;
                nextImage = static_cast<size_t>(index) + 1;
                // 早期图片来源和描述可能直接位于段内；统一归入资产，段内描述仍可供生图记录使用。
                if (segment.contains("file") || segment.contains("url")) {
                    if (static_cast<u64>(index) > images.size())
                        throw DbError("历史图片来源索引无效，无法迁移");
                    if (static_cast<size_t>(index) == images.size())
                        images.push_back(json::object());
                    auto &image = images[static_cast<size_t>(index)];
                    if (!atOrNull(image, "source").is_object())
                        image["source"] = {{"file", getStr(segment, "file")}, {"url", getStr(segment, "url")}};
                    for (const auto *key: {"description", "recognition_status"})
                        if (!image.contains(key) && segment.contains(key))
                            image[key] = segment[key];
                    segment.erase("file");
                    segment.erase("url");
                }
            }
            record["segments"] = std::move(segments);
            if (!images.empty())
                record["assets"]["images"] = std::move(images);
            for (const auto *key: {"text", "images", "faces", "notifications"})
                record.erase(key);
            return record;
        }

        auto migrateMemory(const std::string &content) -> json {
            json parsed;
            const bool structured = tryParseJson(content, parsed) && parsed.is_object() &&
                                    atOrNull(parsed, "aliases").is_array();
            json result = structured ? parsed : json{{"aliases", json::array()}};
            for (auto &alias: result["aliases"])
                renameUserId(alias);
            json pending = json::array();
            std::unordered_set<std::string> seen;
            const auto append = [&](const std::string &value) {
                auto text = trim(value);
                if (!text.empty() && seen.insert(text).second)
                    pending.push_back(std::move(text));
            };
            if (structured) {
                for (const auto *key: {"recent_events", "legacy_entries", "pending_entries"}) {
                    const auto &entries = atOrNull(parsed, key);
                    if (!entries.is_null() && (!entries.is_array() ||
                          !std::ranges::all_of(entries, [](const auto &entry) { return entry.is_string(); })))
                        throw DbError("历史会话记忆条目格式无效，无法迁移");
                    for (const auto &entry: entries)
                        append(entry.template get<std::string>());
                }
            } else if (parsed.is_array() &&
                       std::ranges::all_of(parsed, [](const auto &entry) { return entry.is_string(); })) {
                for (const auto &entry: parsed)
                    append(entry.get<std::string>());
            } else {
                for (size_t start = 0; start < content.size();) {
                    const auto end = content.find('\n', start);
                    const auto stop = end == std::string::npos ? content.size() : end;
                    append(content.substr(start, stop - start));
                    start = stop + 1;
                }
            }
            result.erase("recent_events");
            result.erase("legacy_entries");
            result.erase("pending_entries");
            if (!pending.empty())
                result["pending_entries"] = std::move(pending);
            return result;
        }
    } // namespace

    void migrateV24ToV25(sqlite3 *db) {
        for (const auto *table: {"admins", "blacklisted_users", "person_affinity", "person_memories",
               "person_memory_entries"})
            Statement(db, fmt::format("ALTER TABLE {} RENAME COLUMN qq_number TO user_id", table)).execOrThrow();
        Statement(db, "DROP INDEX IF EXISTS idx_person_memory_entries_qq").execOrThrow();
        Statement(db, "CREATE INDEX idx_person_memory_entries_user_id ON person_memory_entries(user_id, id)").execOrThrow();

        const Statement records(db, "SELECT id, content, role FROM chat_records");
        const Statement updateRecord(db, "UPDATE chat_records SET content=? WHERE id=?");
        while (records.step()) {
            try {
                updateRecord.bind(1, dumpJson(migrateRecord(records.getText(1), records.getText(2))));
            } catch (const std::exception &error) {
                throw DbError(fmt::format("聊天记录 {} 转换失败：{}", records.getInt64(0), error.what()));
            }
            updateRecord.bind(2, records.getInt64(0));
            updateRecord.execOrThrow();
            updateRecord.reset();
        }
        const Statement memories(db, "SELECT group_id, memory_content FROM short_term_memory");
        const Statement updateMemory(db, "UPDATE short_term_memory SET memory_content=? WHERE group_id=?");
        while (memories.step()) {
            try {
                updateMemory.bind(1, dumpJson(migrateMemory(memories.getText(1))));
            } catch (const std::exception &error) {
                throw DbError(fmt::format("会话 {} 记忆转换失败：{}", memories.getInt64(0), error.what()));
            }
            updateMemory.bind(2, memories.getInt64(0));
            updateMemory.execOrThrow();
            updateMemory.reset();
        }
        Statement(db, "DROP TABLE IF EXISTS llm_config").execOrThrow();
        Statement(db, "DROP TABLE IF EXISTS settings").execOrThrow();
    }
} // namespace insoulforge::SchemaMigrator
