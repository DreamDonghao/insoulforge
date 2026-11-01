/// @file V26.cpp
/// @brief 保留数据、索引和自增序列，将用户 ID 从整数转换为文本。
#include <optional>
#include <vector>

#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V26.hpp>

namespace insoulforge::SchemaMigrator {
    void migrateV25ToV26(sqlite3 *db) {
        for (const std::string table:
          {"admins", "blacklisted_users", "person_affinity", "person_memories", "person_memory_entries"}) {
            std::string definition;
            {
                const Statement schema(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?");
                schema.bind(1, table);
                if (!schema.step())
                    throw DbError("用户身份迁移缺少表 " + table);
                definition = schema.getText(0);
            }
            auto body = definition.substr(definition.find('('));
            const auto column = body.find("user_id INTEGER");
            if (column == std::string::npos)
                throw DbError("用户身份迁移遇到非预期表结构 " + table);
            body.replace(column, std::string_view("user_id INTEGER").size(), "user_id TEXT");
            if (body.find("user_id TEXT PRIMARY KEY") != std::string::npos)
                body.replace(body.find("user_id TEXT PRIMARY KEY"), std::string_view("user_id TEXT PRIMARY KEY").size(),
                  "user_id TEXT NOT NULL PRIMARY KEY");
            std::vector<std::string> objects;
            {
                const Statement indexes(db,
                  "SELECT sql FROM sqlite_master WHERE tbl_name=? AND type IN ('index','trigger') AND sql IS NOT NULL");
                indexes.bind(1, table);
                while (indexes.step())
                    objects.push_back(indexes.getText(0));
            }
            std::optional<i64> sequence;
            if (table == "person_memory_entries") {
                const Statement seq(db, "SELECT seq FROM sqlite_sequence WHERE name=?");
                seq.bind(1, table);
                if (seq.step())
                    sequence = seq.getInt64(0);
            }
            std::string columns, values;
            {
                const Statement info(db, fmt::format("PRAGMA table_info({})", table));
                while (info.step()) {
                    const auto name = info.getText(1);
                    if (!columns.empty()) {
                        columns += ',';
                        values += ',';
                    }
                    columns += name;
                    // 旧绑定将 u64 按位写入有符号 INTEGER；%llu 同时恢复超过 INT64_MAX 的身份。
                    values += name == "user_id" ? "CASE WHEN typeof(user_id)='integer' AND user_id<0 THEN "
                                                  "printf('%llu',user_id) ELSE CAST(user_id AS TEXT) END"
                                                : name;
                }
            }
            Statement(db, fmt::format("CREATE TABLE {}_v26 {}", table, body)).execOrThrow();
            Statement(db, fmt::format("INSERT INTO {}_v26 ({}) SELECT {} FROM {}", table, columns, values, table))
              .execOrThrow();
            Statement(db, "DROP TABLE " + table).execOrThrow();
            Statement(db, fmt::format("ALTER TABLE {}_v26 RENAME TO {}", table, table)).execOrThrow();
            for (const auto &sql: objects)
                Statement(db, sql).execOrThrow();
            if (sequence) {
                const Statement restore(db, "UPDATE sqlite_sequence SET seq=max(seq, ?) WHERE name=?");
                restore.bind(1, *sequence);
                restore.bind(2, table);
                restore.execOrThrow();
            }
        }
    }
} // namespace insoulforge::SchemaMigrator
