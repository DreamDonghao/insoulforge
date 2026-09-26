/// @file AdminStore.cpp
/// @brief 管理员存储 - 实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    namespace AdminStore {
        bool isAdmin(const u64 qqNumber) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT 1 FROM admins WHERE qq_number = ?");
            stmt.bind(1, qqNumber);
            return stmt.step();
        }

        void addAdmin(const u64 qqNumber) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "INSERT OR IGNORE INTO admins (qq_number) VALUES (?)");
            stmt.bind(1, qqNumber);
            stmt.exec();
            Logger::info(0, "Admin", fmt::format("已添加管理员: {}", qqNumber));
        }

        void removeAdmin(const u64 qqNumber) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM admins WHERE qq_number = ?");
            stmt.bind(1, qqNumber);
            stmt.exec();
            Logger::info(0, "Admin", fmt::format("已移除管理员: {}", qqNumber));
        }

        std::vector<u64> getAdmins() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<u64> admins;
            const Statement stmt(db.handle(), "SELECT qq_number FROM admins");
            while (stmt.step()) {
                admins.push_back(stmt.getInt64(0));
            }
            return admins;
        }
    } // namespace AdminStore
} // namespace insoulforge
