/// @file V27.hpp
/// @brief 将整数会话键迁移为带类型的文本键；事务由迁移器管理。
#pragma once
struct sqlite3;
namespace insoulforge::SchemaMigrator {
    void migrateV26ToV27(sqlite3 *db);
}
