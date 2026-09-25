/// @file ChatRecordStore.cpp
/// @brief 聊天记录存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <infrastructure/NumericTypes.hpp>

#include <conversation/history/ChatRecordStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    namespace ChatRecordStore {
        void addChatRecord(const u64 sessionId, const std::string &role, const std::string &content) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "INSERT INTO chat_records (group_id, role, content) VALUES (?, ?, ?)");
            stmt.bind(1, sessionId);
            stmt.bind(2, role);
            stmt.bind(3, content);
            stmt.exec();
        }

        auto getChatRecords(const u64 sessionId, const i32 limit) -> std::vector<json> {
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

        auto getChatRecordsWithIds(const u64 sessionId, const i32 limit) -> std::vector<json> {
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

        auto getSessionIds() -> std::vector<u64> {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT DISTINCT group_id FROM chat_records");
            std::vector<u64> sessionIds;
            while (stmt.step()) {
                sessionIds.push_back(static_cast<u64>(stmt.getInt64(0)));
            }
            return sessionIds;
        }

        auto findContentByMessageId(const u64 sessionId, const u64 messageId) -> std::optional<std::string> {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT content FROM chat_records WHERE group_id = ? ORDER BY id DESC");
            stmt.bind(1, sessionId);
            while (stmt.step()) {
                std::string content = stmt.getText(0);
                if (json parsed;
                  tryParseJson(content, parsed) && parseUInt64(getStr(parsed, "message_id")) == messageId) {
                    return content;
                }
            }
            return std::nullopt;
        }

        void updateChatRecord(const i32 recordId, const std::string &content) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "UPDATE chat_records SET content = ? WHERE id = ?");
            stmt.bind(1, content);
            stmt.bind(2, recordId);
            stmt.exec();
        }

        void deleteChatRecord(const i32 recordId) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM chat_records WHERE id = ?");
            stmt.bind(1, recordId);
            stmt.exec();
        }

        void clearSessionChatRecords(const u64 sessionId) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM chat_records WHERE group_id = ?");
            stmt.bind(1, sessionId);
            stmt.exec();
            Logger::info(sessionId, "History", "聊天记录已清空");
        }
    } // namespace ChatRecordStore
} // namespace insoulforge
