/// @file V27.cpp
/// @brief 会话键迁移，保留消息、向量、索引、触发器及自增序列。
#include <optional>
#include <vector>

#include <regex>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V27.hpp>

namespace insoulforge::SchemaMigrator {
    namespace {
        auto legacyKey(u64 value) -> SessionId {
            constexpr u64 flag = 1ULL << 63;
            return value & flag ? SessionId::privateChat(UserId{value & ~flag})
                                : SessionId::group(std::to_string(value));
        }
        auto legacyExpression(std::string_view column) -> std::string {
            return fmt::format("CASE WHEN {0}<0 THEN 'private:'||printf('%llu',{0}&9223372036854775807) ELSE "
                               "'group:'||printf('%llu',{0}) END",
              column);
        }
        void rebuild(sqlite3 *db, const std::string &table, const bool tasks, const bool source) {
            std::string definition;
            std::vector<std::string> objects;
            {
                const Statement schema(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?");
                schema.bind(1, table);
                if (!schema.step())
                    throw DbError("会话迁移缺少表 " + table);
                definition = schema.getText(0);
                const Statement indexes(db,
                  "SELECT sql FROM sqlite_master WHERE tbl_name=? AND type IN ('index','trigger') AND sql IS NOT NULL");
                indexes.bind(1, table);
                while (indexes.step())
                    objects.push_back(indexes.getText(0));
            }
            auto body = definition.substr(definition.find('('));
            if (tasks) {
                body = std::regex_replace(body,
                  std::regex(
                    R"(session_type\s+TEXT\s+NOT\s+NULL\s+CHECK\(session_type\s+IN\s*\('group',\s*'private'\)\),)"),
                  "");
                body = std::regex_replace(body, std::regex(R"(target_id\s+INTEGER)"), "session_id TEXT");
            } else {
                body =
                  std::regex_replace(body, std::regex(source ? R"(source_session\s+INTEGER)" : R"(group_id\s+INTEGER)"),
                    source ? "source_session TEXT" : "session_id TEXT");
                body = std::regex_replace(
                  body, std::regex(R"(session_id TEXT PRIMARY KEY)"), "session_id TEXT NOT NULL PRIMARY KEY");
            }
            std::optional<i64> sequence;
            if (body.find("AUTOINCREMENT") != std::string::npos) {
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
                    if (tasks && name == "session_type")
                        continue;
                    if (!columns.empty()) {
                        columns += ',';
                        values += ',';
                    }
                    if (tasks && name == "target_id") {
                        columns += "session_id";
                        values += "session_type||':'||printf('%llu',target_id)";
                    } else if ((!source && !tasks && name == "group_id") || (source && name == "source_session")) {
                        columns += source ? name : "session_id";
                        values += legacyExpression(name);
                    } else {
                        columns += name;
                        values += name;
                    }
                }
            }
            Statement(db, fmt::format("CREATE TABLE {}_v27 {}", table, body)).execOrThrow();
            Statement(db, fmt::format("INSERT INTO {}_v27 ({}) SELECT {} FROM {}", table, columns, values, table))
              .execOrThrow();
            Statement(db, "DROP TABLE " + table).execOrThrow();
            Statement(db, fmt::format("ALTER TABLE {}_v27 RENAME TO {}", table, table)).execOrThrow();
            for (auto sql: objects) {
                if (tasks)
                    sql = std::regex_replace(sql, std::regex(R"(\btarget_id\b)"), "session_id");
                else if (!source)
                    sql = std::regex_replace(sql, std::regex(R"(\bgroup_id\b)"), "session_id");
                Statement(db, sql).execOrThrow();
            }
            if (sequence) {
                const Statement restore(db, "UPDATE sqlite_sequence SET seq=max(seq,?) WHERE name=?");
                restore.bind(1, *sequence);
                restore.bind(2, table);
                restore.execOrThrow();
            }
        }
    } // namespace
    void migrateV26ToV27(sqlite3 *db) {
        for (const std::string table:
          {"group_config", "enabled_groups", "chat_records", "short_term_memory", "long_term_memory"})
            rebuild(db, table, false, false);
        rebuild(db, "person_memory_entries", false, true);
        rebuild(db, "scheduled_tasks", true, false);
        const Statement records(db, "SELECT id,content FROM chat_records");
        const Statement update(db, "UPDATE chat_records SET content=? WHERE id=?");
        while (records.step()) {
            auto record = json::parse(records.getText(1), nullptr, false);
            if (!record.is_object() || !record.contains("session_id"))
                continue;
            const auto &value = record["session_id"];
            if (value.is_string() && SessionId::parse(value.get_ref<const std::string &>()))
                continue;
            u64 old{};
            if (value.is_number_integer())
                old = value.is_number_unsigned() ? value.get<u64>() : static_cast<u64>(value.get<i64>());
            else if (value.is_string()) {
                const auto parsed = UserId::parse(value.get_ref<const std::string &>());
                if (!parsed || !parsed->toU64())
                    throw DbError(fmt::format("聊天记录 {} 的会话 ID 无效", records.getInt64(0)));
                old = *parsed->toU64();
            } else
                throw DbError(fmt::format("聊天记录 {} 的会话 ID 无效", records.getInt64(0)));
            record["session_id"] = legacyKey(old).toString();
            update.bind(1, dumpJson(record));
            update.bind(2, records.getInt64(0));
            update.execOrThrow();
            update.reset();
        }
    }
} // namespace insoulforge::SchemaMigrator
