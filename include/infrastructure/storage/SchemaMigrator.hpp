/// @file SchemaMigrator.hpp
/// @brief 数据库 Schema 版本迁移
/// @details 基于 PRAGMA user_version 的版本迁移：
///          - 版本号存于 SQLite 的 user_version
///          - 每个版本迁移是一个有序步骤，启动时从当前版本逐个执行到最新版本
///          - 全新数据库直接创建最新 Schema 并写入最新版本号
///          - 每个迁移步骤在独立事务中执行，失败则回滚并抛出异常

#pragma once

#include <sqlite3.h>

#include <infrastructure/NumericTypes.hpp>


/// @brief Schema 版本迁移器
namespace insoulforge::SchemaMigrator {
    /// @brief 最低可直接升级的数据库版本，对应 InSoulForge v1.7.0 正式版。
    inline constexpr i32 kMinimumSupportedVersion = 24;

    /// @brief 当前代码对应的最新 Schema 版本号
    inline constexpr i32 kLatestVersion = 27;

    /// @brief 执行迁移：检查当前版本并按序应用所有待执行步骤
    /// @throws DbError 数据库版本不受支持，或建库/迁移失败（已回滚）。
    void migrate(sqlite3 *db);
} // namespace insoulforge::SchemaMigrator
