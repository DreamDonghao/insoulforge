/// @file V26.hpp
/// @brief 将用户身份存储转换为文本；由调用者管理事务。
#pragma once
struct sqlite3;
namespace insoulforge::SchemaMigrator {
    void migrateV25ToV26(sqlite3 *db);
}
