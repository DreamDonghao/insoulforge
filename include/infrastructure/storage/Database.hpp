/// @file Database.hpp
/// @brief SQLite 数据库连接管理
/// @author donghao
/// @date 2026-04-02
/// @details 仅负责数据库连接的生命周期（打开、迁移、关闭），
///          各领域的数据访问由 storage/ 下的 Store 类承担

#pragma once
#include <shared_mutex>
#include <sqlite3.h>
#include <string>

namespace insoulforge {
    /// @brief SQLite 数据库连接管理类
    class Database {
    public:
        static Database &instance();

        /// @brief 打开数据库并执行 Schema 迁移
        void initialize(const std::string &dbPath);

        /// @brief 关闭数据库
        void close();

        /// @brief 获取底层连接（配合 mutex() 加锁使用）
        [[nodiscard]] sqlite3 *handle() const { return m_db; }

        /// @brief 全局读写锁：读操作用 shared_lock，写操作用 unique_lock
        [[nodiscard]] std::shared_mutex &mutex() const { return m_mutex; }

    private:
        Database() = default;

        ~Database();

        sqlite3 *m_db = nullptr;
        mutable std::shared_mutex m_mutex;
    };
} // namespace insoulforge