/// @file Statement.hpp
/// @brief SQLite Statement RAII 封装
/// @author donghao
/// @date 2026-08-30

#pragma once
#include <concepts>
#include <cstdint>
#include <infrastructure/logging/Logger.hpp>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace insoulforge {
    /// @brief 数据库错误异常
    class DbError : public std::runtime_error {
    public:
        explicit DbError(std::string_view msg) : std::runtime_error(std::string(msg)) {}
    };

    /// @brief SQLite Statement RAII 封装，自动管理 sqlite3_stmt 生命周期
    class Statement {
    public:
        Statement(sqlite3 *db, std::string_view sql) : m_db(db) {
            if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &m_stmt, nullptr) != SQLITE_OK) {
                std::string err = sqlite3_errmsg(db);
                Logger::error(0, "Storage", fmt::format("SQL 准备失败: {} - {}", sql, err));
                throw DbError(err);
            }
        }

        ~Statement() {
            if (m_stmt)
                sqlite3_finalize(m_stmt);
        }

        Statement(const Statement &) = delete;

        Statement &operator=(const Statement &) = delete;

        Statement(Statement &&other) noexcept :
            m_db(std::exchange(other.m_db, nullptr)), m_stmt(std::exchange(other.m_stmt, nullptr)) {}

        Statement &operator=(Statement &&other) noexcept {
            if (this != &other) {
                if (m_stmt)
                    sqlite3_finalize(m_stmt);
                m_db = std::exchange(other.m_db, nullptr);
                m_stmt = std::exchange(other.m_stmt, nullptr);
            }
            return *this;
        }

        void bind(const int idx, std::integral auto v) const noexcept {
            sqlite3_bind_int64(m_stmt, idx, static_cast<int64_t>(v));
        }

        void bind(const int idx, std::floating_point auto v) const noexcept {
            sqlite3_bind_double(m_stmt, idx, static_cast<double>(v));
        }

        void bind(const int idx, const std::string &v) const noexcept {
            sqlite3_bind_text(m_stmt, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }

        void bind(int idx, const std::string_view v) const noexcept {
            sqlite3_bind_text(m_stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }

        void bind(int idx, const char *v) const noexcept { sqlite3_bind_text(m_stmt, idx, v, -1, SQLITE_TRANSIENT); }

        void bind(int idx, const std::vector<uint8_t> &data) const noexcept {
            if (data.empty()) {
                sqlite3_bind_null(m_stmt, idx);
            } else {
                sqlite3_bind_blob(m_stmt, idx, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
            }
        }

        void bindNull(int idx) const noexcept { sqlite3_bind_null(m_stmt, idx); }

        /// @brief 推进一步：true=有行可读，false=完成或出错（错误仅记日志）
        [[nodiscard]] bool step() const noexcept {
            int rc = sqlite3_step(m_stmt);
            if (rc == SQLITE_ROW)
                return true;
            if (rc == SQLITE_DONE)
                return false;
            Logger::error(0, "Storage", fmt::format("SQL 执行失败: {}", sqlite3_errmsg(m_db)));
            return false;
        }

        void exec() const noexcept { std::ignore = step(); }

        /// @brief 执行不返回行的语句，失败时抛出 DbError
        /// @details 事务内必须中止并回滚的写入应使用本函数；尽力而为的写入可继续使用 exec()。
        void execOrThrow() const {
            if (sqlite3_step(m_stmt) != SQLITE_DONE) {
                throw DbError(sqlite3_errmsg(m_db));
            }
        }

        void reset() const noexcept {
            sqlite3_reset(m_stmt);
            sqlite3_clear_bindings(m_stmt);
        }

        [[nodiscard]] int64_t getInt64(int col) const noexcept { return sqlite3_column_int64(m_stmt, col); }

        [[nodiscard]] int getInt(int col) const noexcept { return sqlite3_column_int(m_stmt, col); }

        [[nodiscard]] double getDouble(int col) const noexcept { return sqlite3_column_double(m_stmt, col); }

        [[nodiscard]] std::string getText(int col) const noexcept {
            const auto *p = sqlite3_column_text(m_stmt, col);
            return p ? reinterpret_cast<const char *>(p) : "";
        }

        [[nodiscard]] bool isNull(int col) const noexcept { return sqlite3_column_type(m_stmt, col) == SQLITE_NULL; }

        [[nodiscard]] std::vector<uint8_t> getBlob(int col) const noexcept {
            const auto *p = static_cast<const uint8_t *>(sqlite3_column_blob(m_stmt, col));
            int size = sqlite3_column_bytes(m_stmt, col);
            if (!p || size <= 0)
                return {};
            return {p, p + size};
        }

        [[nodiscard]] static int64_t lastInsertRowId(sqlite3 *db) noexcept { return sqlite3_last_insert_rowid(db); }

        [[nodiscard]] static int changes(sqlite3 *db) noexcept { return sqlite3_changes(db); }

    private:
        sqlite3 *m_db;
        sqlite3_stmt *m_stmt = nullptr;
    };
} // namespace insoulforge
