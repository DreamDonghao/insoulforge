/// @file BlacklistStore.cpp
/// @brief 全局用户黑名单持久化实现

#include <fmt/format.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/access/BlacklistStore.hpp>

namespace insoulforge::BlacklistStore {
    auto contains(const UserId &userId) -> bool {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        const Statement statement(database.handle(), "SELECT 1 FROM blacklisted_users WHERE user_id = ?");
        statement.bind(1, userId.toString());
        return statement.step();
    }

    void add(const UserId &userId) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(), "INSERT OR IGNORE INTO blacklisted_users (user_id) VALUES (?)");
        statement.bind(1, userId.toString());
        statement.exec();
        Logger::info(std::nullopt, "Blacklist", fmt::format("已添加: {}", userId));
    }

    void remove(const UserId &userId) {
        const auto &database = Database::instance();
        std::unique_lock lock(database.mutex());
        const Statement statement(database.handle(), "DELETE FROM blacklisted_users WHERE user_id = ?");
        statement.bind(1, userId.toString());
        statement.exec();
        Logger::info(std::nullopt, "Blacklist", fmt::format("已移除: {}", userId));
    }

    auto getAll() -> std::vector<UserId> {
        const auto &database = Database::instance();
        std::shared_lock lock(database.mutex());
        std::vector<UserId> users;
        const Statement statement(database.handle(), "SELECT user_id FROM blacklisted_users ORDER BY user_id");
        while (statement.step()) {
            users.emplace_back(statement.getText(0));
        }
        return users;
    }
} // namespace insoulforge::BlacklistStore
