/// @file SessionStore.cpp
/// @brief 会话统计与启用状态的数据库实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/session/SessionStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    namespace SessionStore {
        SessionConfig getSessionConfig(const u64 sessionId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            SessionConfig config;
            const Statement stmt(db.handle(), "SELECT all_mes_count FROM group_config WHERE group_id = ?");
            stmt.bind(1, sessionId);
            if (stmt.step()) {
                config.allMesCount = stmt.getInt64(0);
            }
            return config;
        }

        void saveSessionConfig(const u64 sessionId, const SessionConfig &config) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(
              db.handle(), "INSERT OR REPLACE INTO group_config (group_id, all_mes_count) VALUES (?, ?)");
            stmt.bind(1, sessionId);
            stmt.bind(2, config.allMesCount);
            stmt.exec();
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
                Statement insert(
                  db.handle(), "INSERT OR REPLACE INTO group_config (group_id, all_mes_count) VALUES (?, ?)");
                insert.bind(1, sessionId);
                insert.bind(2, 1);
                insert.exec();
            }
        }

        bool hasSessionConfig(const u64 sessionId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT 1 FROM group_config WHERE group_id = ?");
            stmt.bind(1, sessionId);
            return stmt.step();
        }

        bool isSessionEnabled(const u64 sessionId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT enabled FROM enabled_groups WHERE group_id = ?");
            stmt.bind(1, sessionId);
            return stmt.step() && stmt.getInt(0) == 1;
        }

        void enableSession(const u64 sessionId) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(
              db.handle(), "INSERT OR REPLACE INTO enabled_groups (group_id, enabled) VALUES (?, 1)");
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

        std::vector<u64> getEnabledGroups() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<u64> groups;
            const Statement stmt(db.handle(), "SELECT group_id FROM enabled_groups WHERE enabled = 1");
            while (stmt.step()) {
                groups.push_back(stmt.getInt64(0));
            }
            return groups;
        }

        std::vector<std::tuple<u64, std::string, i32>> getSessionsWithChatRecords() {
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

        std::vector<std::tuple<u64, std::string, bool, i32>> getAllSessionsWithStatus() {
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

        std::string getSessionName(const u64 sessionId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT group_name FROM enabled_groups WHERE group_id = ?");
            stmt.bind(1, sessionId);
            return stmt.step() ? stmt.getText(0) : "";
        }
    } // namespace SessionStore
} // namespace insoulforge
