/// @file Database.hpp
/// @brief SQLite 数据库连接管理
/// @details 管理数据库连接的打开、迁移和关闭；各功能模块的 Store 负责具体读写。

#pragma once
#include <shared_mutex>
#include <sqlite3.h>
#include <string>

namespace insoulforge {
    /// @brief SQLite 数据库连接管理类
    class Database {
    public:
        static auto instance() -> Database &;

        /// @brief 打开数据库并执行 Schema 迁移
        void initialize(const std::string &dbPath);

        /// @brief 关闭数据库
        void close();

        /// @brief 获取底层连接（配合 mutex() 加锁使用）
        [[nodiscard]] auto handle() const -> sqlite3 * { return m_db; }

        /// @brief 全局读写锁：读操作用 shared_lock，写操作用 unique_lock
        [[nodiscard]] auto mutex() const -> std::shared_mutex & { return m_mutex; }

    private:
        Database() = default;

        ~Database();

        sqlite3 *m_db = nullptr;
        mutable std::shared_mutex m_mutex;
    };
} // namespace insoulforge
