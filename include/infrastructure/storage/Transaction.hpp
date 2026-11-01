/// @file Transaction.hpp
/// @brief SQLite 写事务的作用域管理。
#pragma once

#include <string_view>

#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    /// @brief 构造时开始 IMMEDIATE 事务，未显式结束的事务在析构时尝试回滚。
    /// @note 不持有数据库或互斥锁；调用方须先加锁，且连接和锁必须覆盖事务生命周期。
    /// 不支持嵌套事务。BEGIN 失败不会回滚调用方已有的事务。
    class Transaction {
    public:
        /// @throws DbError 无法开始事务。
        explicit Transaction(sqlite3 *database) : m_database(database) {
            Statement(m_database, "BEGIN IMMEDIATE").execOrThrow();
            m_active = true;
        }

        /// @brief 回滚未结束的事务；不抛异常，以免覆盖正在传播的原始错误。
        ~Transaction() noexcept {
            if (m_active)
                sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
        }

        Transaction(const Transaction &) = delete;
        auto operator=(const Transaction &) -> Transaction & = delete;
        Transaction(Transaction &&) = delete;
        auto operator=(Transaction &&) -> Transaction & = delete;

        /// @brief 显式提交；失败时仍由析构函数尝试回滚。
        /// @throws DbError 提交失败或事务已经结束。
        void commit() { finish("COMMIT"); }

        /// @brief 显式回滚，供需要报告回滚错误的提前返回路径使用。
        /// @throws DbError 回滚失败或事务已经结束。
        void rollback() { finish("ROLLBACK"); }

    private:
        void finish(std::string_view sql) {
            if (!m_active)
                throw DbError("事务已经结束");
            Statement(m_database, sql).execOrThrow();
            m_active = false;
        }

        sqlite3 *m_database;
        bool m_active = false;
    };
} // namespace insoulforge
