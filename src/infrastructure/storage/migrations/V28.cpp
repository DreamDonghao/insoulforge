/// @file V28.cpp
/// @brief 只转换消息的结构化系统发送者，不修改正文、工具参数或真实用户数据。

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V28.hpp>

namespace insoulforge::SchemaMigrator {
    void migrateV27ToV28(sqlite3 *db) {
        const Statement records(db, "SELECT id,content FROM chat_records");
        const Statement update(db, "UPDATE chat_records SET content=? WHERE id=?");
        while (records.step()) {
            auto record = json::parse(records.getText(1), nullptr, false);
            // 旧标记只在迁移边界识别，运行时统一按 system 判断。
            if (getStr(atOrNull(record, "sender"), "user_id") != "10000000000")
                continue;
            record["sender"]["user_id"] = "system";
            update.bind(1, dumpJson(record));
            update.bind(2, records.getInt64(0));
            update.execOrThrow();
            update.reset();
        }
    }
} // namespace insoulforge::SchemaMigrator
