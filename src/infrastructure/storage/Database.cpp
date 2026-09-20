/// @file Database.cpp
/// @brief SQLite 数据库连接管理 - 实现
/// @author donghao
/// @date 2026-04-02

#include <filesystem>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <spdlog/spdlog.h>

namespace insoulforge {
    Database &Database::instance() {
        static Database db;
        return db;
    }

    Database::~Database() { close(); }

    void Database::initialize(const std::string &dbPath) {
        // 创建数据目录
        const std::filesystem::path p(dbPath);
        if (p.has_parent_path() && !std::filesystem::exists(p.parent_path())) {
            std::filesystem::create_directories(p.parent_path());
        }

        // 打开数据库（仅启动期单线程调用，不加全局锁：
        // 迁移在自身事务中执行）
        if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
            spdlog::error("无法打开数据库: {}", sqlite3_errmsg(m_db));
            return;
        }

        spdlog::info("数据库已打开: {}", dbPath);
        SchemaMigrator::migrate(m_db);
        spdlog::info("数据库初始化完成");
    }

    void Database::close() {
        std::unique_lock lock(m_mutex);
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
            spdlog::info("数据库已关闭");
        }
    }
} // namespace insoulforge
