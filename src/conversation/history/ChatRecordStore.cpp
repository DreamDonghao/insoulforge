/// @file ChatRecordStore.cpp
/// @brief 聊天记录存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <algorithm>
#include <conversation/history/ChatRecordStore.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <ranges>
#include <spdlog/spdlog.h>

namespace insoulforge {
    namespace ChatRecordStore {
        void addChatRecord(const uint64_t sessionId, const std::string &role, const std::string &content) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "INSERT INTO chat_records (group_id, role, content) VALUES (?, ?, ?)");
            stmt.bind(1, sessionId);
            stmt.bind(2, role);
            stmt.bind(3, content);
            stmt.exec();
        }

        std::vector<json> getChatRecords(const uint64_t sessionId, const int limit) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<json> records;

            const Statement stmt(
              db.handle(), "SELECT role, content FROM chat_records WHERE group_id = ? ORDER BY id DESC LIMIT ?");
            stmt.bind(1, sessionId);
            stmt.bind(2, limit);

            while (stmt.step()) {
                json record;
                record["role"] = stmt.getText(0);
                record["content"] = stmt.getText(1);
                records.push_back(record);
            }

            std::ranges::reverse(records);
            return records;
        }

        std::vector<json> getChatRecordsWithIds(const uint64_t sessionId, const int limit) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<json> records;

            const Statement stmt(
              db.handle(), "SELECT id, role, content FROM chat_records WHERE group_id = ? ORDER BY id DESC LIMIT ?");
            stmt.bind(1, sessionId);
            stmt.bind(2, limit);

            while (stmt.step()) {
                json record;
                record["id"] = stmt.getInt64(0);
                record["role"] = stmt.getText(1);
                record["content"] = stmt.getText(2);
                records.push_back(record);
            }

            return records;
        }

        std::vector<uint64_t> getSessionIds() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT DISTINCT group_id FROM chat_records");
            std::vector<uint64_t> sessionIds;
            while (stmt.step()) {
                sessionIds.push_back(static_cast<uint64_t>(stmt.getInt64(0)));
            }
            return sessionIds;
        }

        std::optional<std::string> findContentByMessageId(const uint64_t sessionId, const uint64_t messageId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT content FROM chat_records WHERE group_id = ? ORDER BY id DESC");
            stmt.bind(1, sessionId);
            while (stmt.step()) {
                std::string content = stmt.getText(0);
                json parsed;
                if (tryParseJson(content, parsed) && parseUInt64(getStr(parsed, "message_id")) == messageId) {
                    return content;
                }
            }
            return std::nullopt;
        }

        void updateChatRecord(const int recordId, const std::string &content) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "UPDATE chat_records SET content = ? WHERE id = ?");
            stmt.bind(1, content);
            stmt.bind(2, recordId);
            stmt.exec();
        }

        void deleteChatRecord(const int recordId) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM chat_records WHERE id = ?");
            stmt.bind(1, recordId);
            stmt.exec();
        }

        void clearSessionChatRecords(const uint64_t sessionId) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM chat_records WHERE group_id = ?");
            stmt.bind(1, sessionId);
            stmt.exec();
            spdlog::info("已清空群 {} 的聊天记录", sessionId);
        }
    } // namespace ChatRecordStore
} // namespace insoulforge
