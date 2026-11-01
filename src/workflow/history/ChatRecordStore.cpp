/// @file ChatRecordStore.cpp
/// @brief 聊天记录存储 - 实现

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/Transaction.hpp>
#include <workflow/history/ChatRecordStore.hpp>

namespace insoulforge::ChatRecordStore {
    void replaceSessionChatRecords(const Database &db, const SessionId &sessionId, const json &messages) {
        std::unique_lock lock(db.mutex());
        auto *handle = db.handle();
        Transaction transaction(handle);
        const Statement remove(handle, "DELETE FROM chat_records WHERE session_id = ?");
        remove.bind(1, sessionId.toString());
        remove.execOrThrow();

        const Statement insert(handle, "INSERT INTO chat_records (session_id, role, content) VALUES (?, ?, ?)");
        for (const auto &message: messages) {
            insert.bind(1, sessionId.toString());
            insert.bind(2, getStr(atOrNull(message, "sender"), "user_id") == "self" ? "assistant" : "user");
            insert.bind(3, dumpJson(message));
            insert.execOrThrow();
            insert.reset();
        }
        transaction.commit();
    }

    void addChatRecord(
      const Database &db, const SessionId &sessionId, const std::string &role, const std::string &content) {
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT INTO chat_records (session_id, role, content) VALUES (?, ?, ?)");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, role);
        stmt.bind(3, content);
        stmt.exec();
    }

    auto getChatRecords(const Database &db, const SessionId &sessionId, const i32 limit) -> std::vector<json> {
        std::shared_lock lock(db.mutex());
        std::vector<json> records;

        const Statement stmt(
          db.handle(), "SELECT role, content FROM chat_records WHERE session_id = ? ORDER BY id DESC LIMIT ?");
        stmt.bind(1, sessionId.toString());
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

    auto getChatRecordsWithIds(const SessionId &sessionId, const i32 limit) -> std::vector<json> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<json> records;

        const Statement stmt(
          db.handle(), "SELECT id, role, content FROM chat_records WHERE session_id = ? ORDER BY id DESC LIMIT ?");
        stmt.bind(1, sessionId.toString());
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

    auto getSessionIds(const Database &db) -> std::vector<SessionId> {
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT DISTINCT session_id FROM chat_records");
        std::vector<SessionId> sessionIds;
        while (stmt.step()) {
            sessionIds.push_back(SessionId::parse(stmt.getText(0)).value());
        }
        return sessionIds;
    }

    auto findContentByMessageId(const SessionId &sessionId, const u64 messageId) -> std::optional<std::string> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT content FROM chat_records WHERE session_id = ? ORDER BY id DESC");
        stmt.bind(1, sessionId.toString());
        while (stmt.step()) {
            std::string content = stmt.getText(0);
            if (json parsed; tryParseJson(content, parsed) && parseUInt64(getStr(parsed, "message_id")) == messageId) {
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

    void clearSessionChatRecords(const Database &db, const SessionId &sessionId) {
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM chat_records WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        stmt.exec();
        Logger::info(sessionId, "History", "聊天记录已清空");
    }

    void addChatRecord(const SessionId &sessionId, const std::string &role, const std::string &content) {
        addChatRecord(Database::instance(), sessionId, role, content);
    }
    auto getChatRecords(const SessionId &sessionId, const i32 limit) -> std::vector<json> {
        return getChatRecords(Database::instance(), sessionId, limit);
    }
    auto getSessionIds() -> std::vector<SessionId> { return getSessionIds(Database::instance()); }
    void clearSessionChatRecords(const SessionId &sessionId) {
        clearSessionChatRecords(Database::instance(), sessionId);
    }
} // namespace insoulforge::ChatRecordStore
