/// @file Database.cpp
/// @brief SQLite 数据库连接管理 - 实现

#include <filesystem>
#include <memory>

#include <spdlog/spdlog.h>

#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    auto Database::instance() -> Database & {
        static Database db;
        return db;
    }

    Database::~Database() { close(); }

    auto Database::initialize(const std::string &dbPath) -> std::expected<void, std::string> {
        // 创建数据目录
        const std::filesystem::path p(dbPath);
        if (p.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(p.parent_path(), error);
            if (error) {
                return std::unexpected("无法创建数据库目录: " + error.message());
            }
        }

        // 打开数据库（仅启动期单线程调用，不加全局锁：
        // 迁移在自身事务中执行）
        sqlite3 *rawConnection = nullptr;
        const auto openResult = sqlite3_open(dbPath.c_str(), &rawConnection);
        auto connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{rawConnection, sqlite3_close};
        if (openResult != SQLITE_OK) {
            const auto error = fmt::format(
              "无法打开数据库 {}: {}", dbPath, connection ? sqlite3_errmsg(connection.get()) : "无法分配数据库连接");
            return std::unexpected(error);
        }
        try {
            SchemaMigrator::migrate(connection.get());
        } catch (const DbError &error) {
            return std::unexpected(fmt::format("数据库迁移失败: {}", error.what()));
        }
        if (m_db && sqlite3_close(m_db) != SQLITE_OK) {
            return std::unexpected("已有数据库连接仍被使用，无法替换");
        }
        m_db = connection.release();
        Logger::info(0, "Storage", fmt::format("数据库初始化完成"));
        return {};
    }

    void Database::close() {
        std::unique_lock lock(m_mutex);
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
            Logger::info(0, "Storage", fmt::format("数据库已关闭"));
        }
    }
} // namespace insoulforge
