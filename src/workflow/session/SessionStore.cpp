/// @file SessionStore.cpp
/// @brief 会话统计与启用状态的数据库实现

#include <stdexcept>

#include <fmt/format.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/Transaction.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::SessionStore {
    void initializeSession(const SessionId &sessionId, const bool defaultPrivateEnabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        {
            const Statement existing(db.handle(), "SELECT 1 FROM group_config WHERE session_id = ?");
            existing.bind(1, sessionId.toString());
            if (existing.step())
                return;
        }
        auto *handle = db.handle();
        Transaction transaction(handle);
        const Statement config(handle, "INSERT INTO group_config (session_id) VALUES (?)");
        config.bind(1, sessionId.toString());
        config.execOrThrow();
        if (sessionId.isPrivate()) {
            const Statement status(handle,
              "INSERT INTO enabled_groups (session_id, enabled) VALUES (?, ?) ON CONFLICT(session_id) DO NOTHING");
            status.bind(1, sessionId.toString());
            status.bind(2, defaultPrivateEnabled ? 1 : 0);
            status.execOrThrow();
        }
        transaction.commit();
    }

    auto getSessionConfig(const SessionId &sessionId) -> SessionConfig {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        SessionConfig config;
        const Statement stmt(db.handle(),
          "SELECT all_mes_count, send_group_card, auto_reply, additional_prompt, "
          "command_enabled, cross_session_sharing FROM group_config WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
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

    void saveSessionConfig(const SessionId &sessionId, const SessionConfig &config) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT OR REPLACE INTO group_config (session_id, all_mes_count, "
          "send_group_card, auto_reply, additional_prompt, command_enabled, cross_session_sharing) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, config.allMesCount);
        stmt.bind(3, config.sendGroupCard ? 1 : 0);
        stmt.bind(4, config.autoReply ? 1 : 0);
        stmt.bind(5, config.additionalPrompt);
        stmt.bind(6, config.commandEnabled ? 1 : 0);
        stmt.bind(7, config.crossSessionSharing ? 1 : 0);
        stmt.exec();
    }

    void setSendGroupCard(const SessionId &sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (session_id, send_group_card) VALUES (?, ?) "
          "ON CONFLICT(session_id) DO UPDATE SET send_group_card = excluded.send_group_card");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setAdditionalPrompt(const SessionId &sessionId, const std::string_view prompt) {
        if (prompt.size() > kMaxAdditionalPromptBytes)
            throw std::invalid_argument("群附加提示词不能超过 16000 字节");
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (session_id, additional_prompt) VALUES (?, ?) "
          "ON CONFLICT(session_id) DO UPDATE SET additional_prompt = excluded.additional_prompt");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, prompt);
        stmt.exec();
    }

    void setAutoReply(const SessionId &sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT INTO group_config (session_id, auto_reply) VALUES (?, ?) "
                                          "ON CONFLICT(session_id) DO UPDATE SET auto_reply = excluded.auto_reply");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setCommandEnabled(const SessionId &sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (session_id, command_enabled) VALUES (?, ?) "
          "ON CONFLICT(session_id) DO UPDATE SET command_enabled = excluded.command_enabled");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    void setCrossSessionSharing(const SessionId &sessionId, const bool enabled) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO group_config (session_id, cross_session_sharing) VALUES (?, ?) "
          "ON CONFLICT(session_id) DO UPDATE SET cross_session_sharing = excluded.cross_session_sharing");
        stmt.bind(1, sessionId.toString());
        stmt.bind(2, enabled ? 1 : 0);
        stmt.exec();
    }

    auto getSharedSessions(const SessionId &receiverId) -> std::vector<std::pair<SessionId, std::string>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "SELECT eg.session_id, eg.group_name FROM enabled_groups eg "
          "JOIN group_config gc ON gc.session_id = eg.session_id "
          "WHERE eg.enabled = 1 AND gc.cross_session_sharing = 1 AND eg.session_id != ? "
          "AND EXISTS (SELECT 1 FROM enabled_groups receiver "
          "JOIN group_config rc ON rc.session_id = receiver.session_id "
          "WHERE receiver.session_id = ? AND receiver.enabled = 1 AND rc.cross_session_sharing = 1) "
          "ORDER BY eg.session_id");
        stmt.bind(1, receiverId.toString());
        stmt.bind(2, receiverId.toString());
        std::vector<std::pair<SessionId, std::string>> result;
        while (stmt.step()) {
            result.emplace_back(SessionId::parse(stmt.getText(0)).value(), stmt.getText(1));
        }
        return result;
    }

    void incrementMessageCount(const SessionId &sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());

        const Statement stmt(
          db.handle(), "UPDATE group_config SET all_mes_count = all_mes_count + 1 WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        stmt.exec();

        if (Statement::changes(db.handle()) == 0) {
            // 首条消息：配置行不存在，直接插入
            const Statement insert(
              db.handle(), "INSERT OR REPLACE INTO group_config (session_id, all_mes_count) VALUES (?, ?)");
            insert.bind(1, sessionId.toString());
            insert.bind(2, 1);
            insert.exec();
        }
    }

    auto hasSessionConfig(const SessionId &sessionId) -> bool {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT 1 FROM group_config WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        return stmt.step();
    }

    auto isSessionEnabled(const SessionId &sessionId) -> bool {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT enabled FROM enabled_groups WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        return stmt.step() && stmt.getInt(0) == 1;
    }

    void enableSession(const SessionId &sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT OR REPLACE INTO enabled_groups (session_id, enabled) VALUES (?, 1)");
        stmt.bind(1, sessionId.toString());
        stmt.exec();
        Logger::info(sessionId, "Session", "已启用");
    }

    void disableSession(const SessionId &sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM enabled_groups WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        stmt.exec();
        Logger::info(sessionId, "Session", "已禁用");
    }

    auto getEnabledSessions() -> std::vector<SessionId> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<SessionId> groups;
        const Statement stmt(db.handle(), "SELECT session_id FROM enabled_groups WHERE enabled = 1");
        while (stmt.step()) {
            groups.push_back(SessionId::parse(stmt.getText(0)).value());
        }
        return groups;
    }

    auto getSessionsWithChatRecords() -> std::vector<std::tuple<SessionId, std::string, i32>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<std::tuple<SessionId, std::string, i32>> groups;
        const Statement stmt(db.handle(), "SELECT cr.session_id, COALESCE(eg.group_name, ''), COUNT(*) as cnt "
                                          "FROM chat_records cr "
                                          "LEFT JOIN enabled_groups eg ON cr.session_id = eg.session_id "
                                          "GROUP BY cr.session_id "
                                          "ORDER BY cnt DESC");
        while (stmt.step()) {
            groups.emplace_back(SessionId::parse(stmt.getText(0)).value(), stmt.getText(1), stmt.getInt(2));
        }
        return groups;
    }

    auto getAllSessionsWithStatus() -> std::vector<std::tuple<SessionId, std::string, bool, i32>> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<std::tuple<SessionId, std::string, bool, i32>> groups;
        const Statement stmt(db.handle(), "SELECT eg.session_id, eg.group_name, eg.enabled, "
                                          "(SELECT COUNT(*) FROM chat_records WHERE session_id = eg.session_id) as cnt "
                                          "FROM enabled_groups eg "
                                          "ORDER BY eg.enabled DESC, cnt DESC");
        while (stmt.step()) {
            groups.emplace_back(
              SessionId::parse(stmt.getText(0)).value(), stmt.getText(1), stmt.getInt(2) == 1, stmt.getInt(3));
        }
        return groups;
    }

    void toggleSessionStatus(const SessionId &sessionId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "UPDATE enabled_groups SET enabled = NOT enabled WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        stmt.exec();
    }

    void updateSessionName(const SessionId &sessionId, const std::string &name) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "UPDATE enabled_groups SET group_name = ? WHERE session_id = ?");
        stmt.bind(1, name);
        stmt.bind(2, sessionId.toString());
        stmt.exec();
        Logger::info(sessionId, "Session", fmt::format("名称已更新: {}", name));
    }

    auto getSessionName(const SessionId &sessionId) -> std::string {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT group_name FROM enabled_groups WHERE session_id = ?");
        stmt.bind(1, sessionId.toString());
        return stmt.step() ? stmt.getText(0) : "";
    }
} // namespace insoulforge::SessionStore
