/// @file V25.hpp
/// @brief 数据库 v24 到 v25 的一次性转换

#pragma once

#include <sqlite3.h>

namespace insoulforge::SchemaMigrator {
    /// @brief 调用方在事务内执行；转换历史消息、会话记忆及用户身份。
    void migrateV24ToV25(sqlite3 *db);
} // namespace insoulforge::SchemaMigrator
