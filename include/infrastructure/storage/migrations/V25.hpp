/// @file V25.hpp
/// @brief 数据库 v24 到 v25 的一次性转换

#pragma once

#include <sqlite3.h>

namespace insoulforge::SchemaMigrator {
    /// @brief 调用方在同一事务内执行；统一历史消息、用户与会话身份、记忆、任务及内部标记。
    void migrateV24ToV25(sqlite3 *db);
} // namespace insoulforge::SchemaMigrator
