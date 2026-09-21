/// @file BlacklistStore.cpp
/// @brief 全局 QQ 黑名单持久化实现

#include <admin/BlacklistStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::BlacklistStore {
    bool contains(const uint64_t qqNumber) {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(database.handle(), "SELECT 1 FROM blacklisted_users WHERE qq_number = ?");
        statement.bind(1, qqNumber);
        return statement.step();
    }

    void add(const uint64_t qqNumber) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(), "INSERT OR IGNORE INTO blacklisted_users (qq_number) VALUES (?)");
        statement.bind(1, qqNumber);
        statement.exec();
        Logger::info(0, "Blacklist", fmt::format("已添加: {}", qqNumber));
    }

    void remove(const uint64_t qqNumber) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(), "DELETE FROM blacklisted_users WHERE qq_number = ?");
        statement.bind(1, qqNumber);
        statement.exec();
        Logger::info(0, "Blacklist", fmt::format("已移除: {}", qqNumber));
    }

    std::vector<uint64_t> getAll() {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        std::vector<uint64_t> users;
        const Statement statement(database.handle(), "SELECT qq_number FROM blacklisted_users ORDER BY qq_number");
        while (statement.step()) {
            users.push_back(statement.getInt64(0));
        }
        return users;
    }
} // namespace insoulforge::BlacklistStore
