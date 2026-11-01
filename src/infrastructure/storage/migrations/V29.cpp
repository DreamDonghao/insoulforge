/// @file V29.cpp
/// @brief 只按记录自身的身份依据转换 self，不用当前账号推测历史身份。

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V29.hpp>

namespace insoulforge::SchemaMigrator {
    void migrateV28ToV29(sqlite3 *db) {
        const Statement records(db, "SELECT id,content,role FROM chat_records");
        const Statement update(db, "UPDATE chat_records SET content=? WHERE id=?");
        while (records.step()) {
            auto record = json::parse(records.getText(1), nullptr, false);
            if (!record.is_object())
                continue;
            auto selfId = getStr(record, "self_id");
            const auto sender = getStr(atOrNull(record, "sender"), "user_id");
            if (selfId.empty() && records.getText(2) == "assistant" && sender != "self" && sender != "system")
                selfId = sender;
            if (selfId.empty() || selfId == "0" || selfId == "self" || selfId == "system")
                continue;
            bool changed = false;
            const auto normalize = [&selfId, &changed](json &identity) {
                if (identity.is_object() && getStr(identity, "user_id") == selfId) {
                    identity["user_id"] = "self";
                    changed = true;
                }
            };
            if (record.contains("sender"))
                normalize(record["sender"]);
            if (record.contains("segments") && record["segments"].is_array()) {
                for (auto &segment: record["segments"]) {
                    const auto type = getStr(segment, "type");
                    if ((type == "at" || type == "poke") && segment.contains("target"))
                        normalize(segment["target"]);
                    if (type == "member_event" && segment.contains("operator"))
                        normalize(segment["operator"]);
                }
            }
            if (!changed)
                continue;
            update.bind(1, dumpJson(record));
            update.bind(2, records.getInt64(0));
            update.execOrThrow();
            update.reset();
        }
    }
} // namespace insoulforge::SchemaMigrator
