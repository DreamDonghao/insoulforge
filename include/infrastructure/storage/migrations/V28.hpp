/// @file V28.hpp
/// @brief 将历史系统发送者转换为 system；事务由迁移器管理。
#pragma once

struct sqlite3;

namespace insoulforge::SchemaMigrator {
    void migrateV27ToV28(sqlite3 *db);
}
