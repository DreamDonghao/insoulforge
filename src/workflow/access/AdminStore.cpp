/// @file AdminStore.cpp
/// @brief 管理员存储 - 实现

#include <fmt/format.h>

#include <infrastructure/UserId.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/access/AdminStore.hpp>

namespace insoulforge::AdminStore {
    auto isAdmin(const UserId &userId) -> bool {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT 1 FROM admins WHERE user_id = ?");
        stmt.bind(1, userId.toString());
        return stmt.step();
    }

    void addAdmin(const UserId &userId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT OR IGNORE INTO admins (user_id) VALUES (?)");
        stmt.bind(1, userId.toString());
        stmt.exec();
        Logger::info(std::nullopt, "Admin", fmt::format("已添加管理员: {}", userId));
    }

    void removeAdmin(const UserId &userId) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "DELETE FROM admins WHERE user_id = ?");
        stmt.bind(1, userId.toString());
        stmt.exec();
        Logger::info(std::nullopt, "Admin", fmt::format("已移除管理员: {}", userId));
    }

    auto getAdmins() -> std::vector<UserId> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        std::vector<UserId> admins;
        const Statement stmt(db.handle(), "SELECT user_id FROM admins");
        while (stmt.step()) {
            admins.emplace_back(stmt.getText(0));
        }
        return admins;
    }
} // namespace insoulforge::AdminStore
