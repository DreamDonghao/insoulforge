/// @file Statement.hpp
/// @brief SQLite Statement RAII 封装

#pragma once

#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>

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
            if (sqlite3_prepare_v2(db, sql.data(), static_cast<i32>(sql.size()), &m_stmt, nullptr) != SQLITE_OK) {
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

        auto operator=(const Statement &) -> Statement & = delete;

        Statement(Statement &&other) noexcept :
            m_db(std::exchange(other.m_db, nullptr)), m_stmt(std::exchange(other.m_stmt, nullptr)) {}

        auto operator=(Statement &&other) noexcept -> Statement & {
            if (this != &other) {
                if (m_stmt)
                    sqlite3_finalize(m_stmt);
                m_db = std::exchange(other.m_db, nullptr);
                m_stmt = std::exchange(other.m_stmt, nullptr);
            }
            return *this;
        }

        void bind(const i32 idx, std::integral auto v) const noexcept {
            sqlite3_bind_int64(m_stmt, idx, static_cast<i64>(v));
        }

        void bind(const i32 idx, std::floating_point auto v) const noexcept {
            sqlite3_bind_double(m_stmt, idx, static_cast<f64>(v));
        }

        void bind(const i32 idx, const std::string &v) const noexcept {
            sqlite3_bind_text(m_stmt, idx, v.c_str(), static_cast<i32>(v.size()), SQLITE_TRANSIENT);
        }

        void bind(i32 idx, const std::string_view v) const noexcept {
            sqlite3_bind_text(m_stmt, idx, v.data(), static_cast<i32>(v.size()), SQLITE_TRANSIENT);
        }

        void bind(i32 idx, const char *v) const noexcept { sqlite3_bind_text(m_stmt, idx, v, -1, SQLITE_TRANSIENT); }

        void bind(i32 idx, const std::vector<u8> &data) const noexcept {
            if (data.empty()) {
                sqlite3_bind_null(m_stmt, idx);
            } else {
                sqlite3_bind_blob(m_stmt, idx, data.data(), static_cast<i32>(data.size()), SQLITE_TRANSIENT);
            }
        }

        void bindNull(i32 idx) const noexcept { sqlite3_bind_null(m_stmt, idx); }

        /// @brief 推进一步：true=有行可读，false=完成或出错（错误仅记日志）
        [[nodiscard]] auto step() const noexcept -> bool {
            i32 rc = sqlite3_step(m_stmt);
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

        [[nodiscard]] auto getInt64(i32 col) const noexcept -> i64 { return sqlite3_column_int64(m_stmt, col); }

        [[nodiscard]] auto getInt(i32 col) const noexcept -> i32 { return sqlite3_column_int(m_stmt, col); }

        [[nodiscard]] auto getDouble(i32 col) const noexcept -> f64 { return sqlite3_column_double(m_stmt, col); }

        [[nodiscard]] auto getText(i32 col) const noexcept -> std::string {
            const auto *p = sqlite3_column_text(m_stmt, col);
            return p ? reinterpret_cast<const char *>(p) : "";
        }

        [[nodiscard]] auto isNull(i32 col) const noexcept -> bool {
            return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
        }

        [[nodiscard]] auto getBlob(i32 col) const noexcept -> std::vector<u8> {
            const auto *p = static_cast<const u8 *>(sqlite3_column_blob(m_stmt, col));
            i32 size = sqlite3_column_bytes(m_stmt, col);
            if (!p || size <= 0)
                return {};
            return {p, p + size};
        }

        [[nodiscard]] static auto lastInsertRowId(sqlite3 *db) noexcept -> i64 { return sqlite3_last_insert_rowid(db); }

        [[nodiscard]] static auto changes(sqlite3 *db) noexcept -> i32 { return sqlite3_changes(db); }

    private:
        sqlite3 *m_db;
        sqlite3_stmt *m_stmt = nullptr;
    };
} // namespace insoulforge
