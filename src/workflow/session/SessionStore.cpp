/// @file SessionStore.cpp
/// @brief 会话统计与启用状态的数据库实现

#include <stdexcept>

#include <fmt/format.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::SessionStore {
    void initializeSession(const u64 sessionId, const bool defaultPrivateEnabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        {
            const Statement existing(db.handle(), "SELECT 1 FROM group_config WHERE group_id = ?");
            existing.bind(1, sessionId);
            if (existing.step()) return;
        }
        auto *handle = db.handle();
        Statement(handle, "BEGIN IMMEDIATE").execOrThrow();
        try {
            const Statement config(handle, "INSERT INTO group_config (group_id) VALUES (?)");
            config.bind(1, sessionId);
            config.execOrThrow();
            if (SessionId::isPrivate(sessionId)) {
                const Statement status(handle,
                  "INSERT INTO enabled_groups (group_id, enabled) VALUES (?, ?) ON CONFLICT(group_id) DO NOTHING");
                status.bind(1, sessionId);
                status.bind(2, defaultPrivateEnabled ? 1 : 0);
                status.execOrThrow();
            }
            Statement(handle, "COMMIT").execOrThrow();
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    auto getSessionConfig(const u64 sessionId) -> SessionConfig {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        SessionConfig config;
        const Statement stmt(db.handle(),
          "SELECT all_mes_count, send_group_card, auto_reply, additional_prompt, "
          "command_enabled, cross_session_sharing FROM group_config WHERE group_id = ?");
        stmt.bind(1, sessionId);
        if (stmt.step()) {
            config.allMesCount = stmt.getInt64(0);
            config.sendGroupCard = stmt.getInt(1) != 0;
            config.autoReply = stmt.getInt(2) != 0;
            config.additionalPrompt = stmt.getText(3);
            config.commandEnabled = stmt.getInt(4) != 0;
            config.crossSessionSharing = stmt.getInt(5) != 0;
        }
        return config;
    }

    void saveSessionConfig(const u64 sessionId, const SessionConfig &config) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT OR REPLACE INTO group_config (group_id, all_mes_count, "
          "send_group_card, auto_reply, additional_prompt, command_enabled, cross_session_sharing) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)");
        stmt.bind(1, sessionId);
        stmt.bind(2, config.allMesCount);
        stmt.bind(3, config.sendGroupCard ? 1 : 0);
        stmt.bind(4, config.autoReply ? 1 : 0);
        stmt.bind(5, config.additionalPrompt);
        stmt.bind(6, config.commandEnabled ? 1 : 0);
        stmt.bind(7, config.crossSessionSharing ? 1 : 0);
        stmt.exec();
    }

    void setSendGroupCard(const u64 sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (group_id, send_group_card) VALUES (?, ?) "
          "ON CONFLICT(group_id) DO UPDATE SET send_group_card = excluded.send_group_card");
        stmt.bind(1, sessionId);
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setAdditionalPrompt(const u64 sessionId, const std::string_view prompt) {
        if (prompt.size() > kMaxAdditionalPromptBytes)
            throw std::invalid_argument("群附加提示词不能超过 16000 字节");
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (group_id, additional_prompt) VALUES (?, ?) "
          "ON CONFLICT(group_id) DO UPDATE SET additional_prompt = excluded.additional_prompt");
        stmt.bind(1, sessionId);
        stmt.bind(2, prompt);
        stmt.exec();
    }

    void setAutoReply(const u64 sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT INTO group_config (group_id, auto_reply) VALUES (?, ?) "
                                          "ON CONFLICT(group_id) DO UPDATE SET auto_reply = excluded.auto_reply");
        stmt.bind(1, sessionId);
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setCommandEnabled(const u64 sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (group_id, command_enabled) VALUES (?, ?) "
          "ON CONFLICT(group_id) DO UPDATE SET command_enabled = excluded.command_enabled");
        stmt.bind(1, sessionId);
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setCrossSessionSharing(const u64 sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (group_id, cross_session_sharing) VALUES (?, ?) "
          "ON CONFLICT(group_id) DO UPDATE SET cross_session_sharing = excluded.cross_session_sharing");
        stmt.bind(1, sessionId);
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    auto getSharedSessions(const u64 receiverId) -> std::vector<std::pair<u64, std::string>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "SELECT eg.group_id, eg.group_name FROM enabled_groups eg "
          "JOIN group_config gc ON gc.group_id = eg.group_id "
          "WHERE eg.enabled = 1 AND gc.cross_session_sharing = 1 AND eg.group_id != ? "
          "AND EXISTS (SELECT 1 FROM enabled_groups receiver "
          "JOIN group_config rc ON rc.group_id = receiver.group_id "
          "WHERE receiver.group_id = ? AND receiver.enabled = 1 AND rc.cross_session_sharing = 1) "
          "ORDER BY eg.group_id");
        stmt.bind(1, receiverId);
        stmt.bind(2, receiverId);
        std::vector<std::pair<u64, std::string>> result;
        while (stmt.step()) {
            result.emplace_back(stmt.getInt64(0), stmt.getText(1));
        }
        return result;
    }

    void incrementMessageCount(const u64 sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());

        const Statement stmt(
          db.handle(), "UPDATE group_config SET all_mes_count = all_mes_count + 1 WHERE group_id = ?");
        stmt.bind(1, sessionId);
        stmt.exec();

        if (Statement::changes(db.handle()) == 0) {
            // 首条消息：配置行不存在，直接插入
            const Statement insert(
              db.handle(), "INSERT OR REPLACE INTO group_config (group_id, all_mes_count) VALUES (?, ?)");
            insert.bind(1, sessionId);
            insert.bind(2, 1);
            insert.exec();
        }
    }

    auto hasSessionConfig(const u64 sessionId) -> bool {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT 1 FROM group_config WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step();
    }

    auto isSessionEnabled(const u64 sessionId) -> bool {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT enabled FROM enabled_groups WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step() && stmt.getInt(0) == 1;
    }

    void enableSession(const u64 sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT OR REPLACE INTO enabled_groups (group_id, enabled) VALUES (?, 1)");
        stmt.bind(1, sessionId);
        stmt.exec();
        Logger::info(sessionId, "Session", "已启用");
    }

    void disableSession(const u64 sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM enabled_groups WHERE group_id = ?");
        stmt.bind(1, sessionId);
        stmt.exec();
        Logger::info(sessionId, "Session", "已禁用");
    }

    auto getEnabledGroups() -> std::vector<u64> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<u64> groups;
        const Statement stmt(db.handle(), "SELECT group_id FROM enabled_groups WHERE enabled = 1");
        while (stmt.step()) {
            groups.push_back(stmt.getInt64(0));
        }
        return groups;
    }

    auto getSessionsWithChatRecords() -> std::vector<std::tuple<u64, std::string, i32>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<std::tuple<u64, std::string, i32>> groups;
        const Statement stmt(db.handle(), "SELECT cr.group_id, COALESCE(eg.group_name, ''), COUNT(*) as cnt "
                                          "FROM chat_records cr "
                                          "LEFT JOIN enabled_groups eg ON cr.group_id = eg.group_id "
                                          "GROUP BY cr.group_id "
                                          "ORDER BY cnt DESC");
        while (stmt.step()) {
            groups.emplace_back(stmt.getInt64(0), stmt.getText(1), stmt.getInt(2));
        }
        return groups;
    }

    auto getAllSessionsWithStatus() -> std::vector<std::tuple<u64, std::string, bool, i32>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<std::tuple<u64, std::string, bool, i32>> groups;
        const Statement stmt(db.handle(), "SELECT eg.group_id, eg.group_name, eg.enabled, "
                                          "(SELECT COUNT(*) FROM chat_records WHERE group_id = eg.group_id) as cnt "
                                          "FROM enabled_groups eg "
                                          "ORDER BY eg.enabled DESC, cnt DESC");
        while (stmt.step()) {
            groups.emplace_back(stmt.getInt64(0), stmt.getText(1), stmt.getInt(2) == 1, stmt.getInt(3));
        }
        return groups;
    }

    void toggleSessionStatus(const u64 sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "UPDATE enabled_groups SET enabled = NOT enabled WHERE group_id = ?");
        stmt.bind(1, sessionId);
        stmt.exec();
    }

    void updateSessionName(const u64 sessionId, const std::string &name) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "UPDATE enabled_groups SET group_name = ? WHERE group_id = ?");
        stmt.bind(1, name);
        stmt.bind(2, sessionId);
        stmt.exec();
        Logger::info(sessionId, "Session", fmt::format("名称已更新: {}", name));
    }

    auto getSessionName(const u64 sessionId) -> std::string {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT group_name FROM enabled_groups WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step() ? stmt.getText(0) : "";
    }
} // namespace insoulforge::SessionStore
