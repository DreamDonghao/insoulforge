/// @file V29.hpp
/// @brief 规范化可确认的历史机器人身份；事务由迁移器管理。
#pragma once

struct sqlite3;

namespace insoulforge::SchemaMigrator {
    void migrateV28ToV29(sqlite3 *db);
}
