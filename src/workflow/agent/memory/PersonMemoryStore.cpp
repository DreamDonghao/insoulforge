/// @file PersonMemoryStore.cpp
/// @brief 全局人物记忆的定向读取。

#include <map>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge::PersonMemoryStore {
    namespace {
        // 调用方持有数据库锁，供读取及事务内快照校验共用。
        auto readPerson(u64 qq) -> json;
        // 查询结果中固定记忆和近期互动总是相邻的两列。
        auto readProfile(const u64 qq, const Statement &row, const i32 memoryColumn = 0) -> json {
            return {{"qq", std::to_string(qq)},
              {"stable_memory", row.isNull(memoryColumn) ? json::array() : json::parse(row.getText(memoryColumn))},
              {"recent_interactions",
                row.isNull(memoryColumn + 1) ? json::array() : json::parse(row.getText(memoryColumn + 1))},
              {"pending_entries", json::array()}};
        }

        // 详情和整理快照共用条目的列顺序：正文、来源会话、UTC 写入时间。
        auto readEntry(const Statement &row, const i32 contentColumn = 0) -> json {
            return {{"content", row.getText(contentColumn)},
              {"source_session", std::to_string(static_cast<u64>(row.getInt64(contentColumn + 1)))},
              {"time", row.getText(contentColumn + 2)}};
        }
    } // namespace

    auto pendingPeople() -> std::vector<u64> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(), "SELECT DISTINCT qq_number FROM person_memory_entries ORDER BY qq_number");
        std::vector<u64> people;
        while (query.step())
            people.push_back(static_cast<u64>(query.getInt64(0)));
        return people;
    }

    auto pendingSnapshot(const u64 qq) -> std::optional<PendingSnapshot> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement profile(
          db.handle(), "SELECT stable_memory, recent_interactions FROM person_memories WHERE qq_number = ?");
        profile.bind(1, qq);
        if (!profile.step())
            return std::nullopt;
        PendingSnapshot snapshot{qq, 0, readProfile(qq, profile)};
        const Statement entries(db.handle(),
          "SELECT id, content, source_session, created_at FROM person_memory_entries WHERE qq_number = ? ORDER BY id");
        entries.bind(1, qq);
        while (entries.step()) {
            snapshot.lastEntryId = entries.getInt64(0);
            auto entry = readEntry(entries, 1);
            entry["id"] = std::to_string(snapshot.lastEntryId);
            snapshot.profile["pending_entries"].push_back(std::move(entry));
        }
        return snapshot.lastEntryId ? std::optional{std::move(snapshot)} : std::nullopt;
    }

    void applyConsolidation(const PendingSnapshot &snapshot, const json &profile) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const auto handle = db.handle();
        Statement(handle, "BEGIN IMMEDIATE").execOrThrow();
        try {
            const auto current = readPerson(snapshot.qq);
            json entries = json::array();
            if (!current.is_null()) {
                for (auto it = current["pending_entries"].rbegin(); it != current["pending_entries"].rend(); ++it)
                    if (parseUInt64(getStr(*it, "id")) <= static_cast<u64>(snapshot.lastEntryId)) entries.push_back(*it);
            }
            if (current.is_null() || current["stable_memory"] != snapshot.profile["stable_memory"] ||
                current["recent_interactions"] != snapshot.profile["recent_interactions"] ||
                entries != snapshot.profile["pending_entries"])
                throw DbError("人物记忆已修改，丢弃过期整理结果");
            const Statement update(
              handle, "UPDATE person_memories SET stable_memory = ?, recent_interactions = ? WHERE qq_number = ?");
            update.bind(1, profile.at("stable_memory").dump());
            update.bind(2, profile.at("recent_interactions").dump());
            update.bind(3, snapshot.qq);
            update.execOrThrow();
            if (Statement::changes(handle) != 1)
                throw DbError("人物档案不存在");
            const Statement remove(handle, "DELETE FROM person_memory_entries WHERE qq_number = ? AND id <= ?");
            remove.bind(1, snapshot.qq);
            remove.bind(2, snapshot.lastEntryId);
            remove.execOrThrow();
            Statement(handle, "COMMIT").execOrThrow();
        } catch (...) {
            sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    auto dailyState() -> DailyState {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(
          db.handle(), "SELECT completed_day, running_day FROM person_memory_daily_state WHERE id = 1");
        return query.step() ? DailyState{query.getText(0), query.getText(1)} : DailyState{};
    }

    void saveDailyState(const DailyState &state) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement update(db.handle(),
          "INSERT INTO person_memory_daily_state (id, completed_day, running_day) VALUES (1, ?, ?) "
          "ON CONFLICT(id) DO UPDATE SET completed_day = excluded.completed_day, running_day = excluded.running_day");
        update.bind(1, state.completedDay);
        update.bind(2, state.runningDay);
        update.execOrThrow();
    }

    auto listPeople() -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(),
          "WITH people AS (SELECT qq_number FROM person_memories UNION SELECT qq_number FROM person_affinity) "
          "SELECT p.qq_number, COALESCE(a.affinity, 0), COUNT(e.id), MAX(e.created_at) "
          "FROM people p LEFT JOIN person_affinity a ON a.qq_number = p.qq_number "
          "LEFT JOIN person_memory_entries e ON e.qq_number = p.qq_number "
          "GROUP BY p.qq_number ORDER BY p.qq_number");
        json result = json::array();
        while (query.step()) {
            result.push_back(
              {{"qq", std::to_string(static_cast<u64>(query.getInt64(0)))}, {"affinity", query.getInt64(1)},
                {"pending_count", query.getInt64(2)}, {"updated_at", query.isNull(3) ? "" : query.getText(3)}});
        }
        return result;
    }

    auto getPerson(const u64 qq) -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        return readPerson(qq);
    }

    namespace {
    auto readPerson(const u64 qq) -> json {
        const auto &db = Database::instance();
        const Statement query(db.handle(),
          "SELECT p.stable_memory, p.recent_interactions, COALESCE(a.affinity, 0) "
          "FROM (SELECT qq_number FROM person_memories UNION SELECT qq_number FROM person_affinity) ids "
          "LEFT JOIN person_memories p ON p.qq_number = ids.qq_number "
          "LEFT JOIN person_affinity a ON a.qq_number = ids.qq_number WHERE ids.qq_number = ?");
        query.bind(1, qq);
        if (!query.step())
            return nullptr;
        json result = {{"qq", std::to_string(qq)}, {"affinity", query.getInt64(2)}};
        result.update(readProfile(qq, query));
        const Statement entries(db.handle(),
          "SELECT content, source_session, created_at, id FROM person_memory_entries WHERE qq_number = ? ORDER BY id DESC");
        entries.bind(1, qq);
        while (entries.step()) {
            auto entry = readEntry(entries);
            entry["id"] = std::to_string(entries.getInt64(3));
            result["pending_entries"].push_back(std::move(entry));
        }
        return result;
    }
    }

    namespace {
        auto matches(const json &current, const json &expected) -> bool {
            if (current.is_null() || !expected.is_object()) return false;
            for (const auto key : {"qq", "affinity", "stable_memory", "recent_interactions", "pending_entries"})
                if (!expected.contains(key) || current.at(key) != expected.at(key)) return false;
            return true;
        }

        auto mutatePerson(const u64 qq, const json &expected, const json *replacement) -> bool {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            auto *handle = db.handle();
            Statement(handle, "BEGIN IMMEDIATE").execOrThrow();
            try {
                if (!matches(readPerson(qq), expected)) {
                    Statement(handle, "ROLLBACK").execOrThrow();
                    return false;
                }
                if (replacement) {
                    const Statement profile(handle, "INSERT INTO person_memories (qq_number, stable_memory, recent_interactions) VALUES (?, ?, ?) ON CONFLICT(qq_number) DO UPDATE SET stable_memory=excluded.stable_memory, recent_interactions=excluded.recent_interactions");
                    profile.bind(1, qq);
                    profile.bind(2, replacement->at("stable_memory").dump());
                    profile.bind(3, replacement->at("recent_interactions").dump());
                    profile.execOrThrow();
                    const Statement score(handle, "INSERT INTO person_affinity (qq_number, affinity) VALUES (?, ?) ON CONFLICT(qq_number) DO UPDATE SET affinity=excluded.affinity");
                    score.bind(1, qq);
                    score.bind(2, replacement->at("affinity").get<i32>());
                    score.execOrThrow();
                    std::unordered_map<std::string, std::string> contents;
                    for (const auto &entry : replacement->at("pending_entries"))
                        contents.emplace(getStr(entry, "id"), getStr(entry, "content"));
                    for (const auto &old : expected.at("pending_entries")) {
                        const auto id = getStr(old, "id");
                        const auto found = contents.find(id);
                        if (found != contents.end() && found->second == getStr(old, "content"))
                            continue;
                        const Statement change(handle, found == contents.end() ?
                            "DELETE FROM person_memory_entries WHERE qq_number=? AND id=?" :
                            "UPDATE person_memory_entries SET content=?3 WHERE qq_number=?1 AND id=?2");
                        change.bind(1, qq);
                        change.bind(2, parseUInt64(id));
                        if (found != contents.end()) change.bind(3, found->second);
                        change.execOrThrow();
                    }
                } else {
                    for (const auto table : {"person_memory_entries", "person_memories", "person_affinity"}) {
                        const Statement remove(handle, std::string("DELETE FROM ") + table + " WHERE qq_number=?");
                        remove.bind(1, qq);
                        remove.execOrThrow();
                    }
                }
                Statement(handle, "COMMIT").execOrThrow();
                return true;
            } catch (...) {
                sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        }
    }

    auto updatePerson(const u64 qq, const json &expected, const json &replacement) -> bool {
        return mutatePerson(qq, expected, &replacement);
    }
    auto deletePerson(const u64 qq, const json &expected) -> bool {
        return mutatePerson(qq, expected, nullptr);
    }

    auto getForSenders(const json &recentConversation) -> json {
        std::vector<u64> senders;
        std::unordered_set<u64> seen;
        for (const auto &record: recentConversation) {
            const auto qq = parseUInt64(getStr(atOrNull(record, "sender"), "qq"));
            if (qq > 0 && qq != SessionId::kSystemAccountId && seen.insert(qq).second)
                senders.push_back(qq);
        }
        json result = json::array();
        if (senders.empty())
            return result;
        std::string placeholders;
        for (size_t index = 0; index < senders.size(); ++index) {
            if (index != 0)
                placeholders += ',';
            placeholders += '?';
        }
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(),
          "SELECT p.qq_number, p.stable_memory, p.recent_interactions, e.id, e.content, e.source_session, e.created_at "
          "FROM person_memories p LEFT JOIN person_memory_entries e ON e.qq_number = p.qq_number "
          "WHERE p.qq_number IN (" +
            placeholders + ") ORDER BY p.qq_number, e.id");
        for (size_t index = 0; index < senders.size(); ++index)
            query.bind(static_cast<i32>(index + 1), senders[index]);
        std::map<u64, json> profiles;
        while (query.step()) {
            const auto qq = static_cast<u64>(query.getInt64(0));
            auto [it, inserted] = profiles.try_emplace(qq);
            if (inserted) {
                it->second = readProfile(qq, query, 1);
            }
            if (!query.isNull(3)) {
                it->second["pending_entries"].push_back(
                  {{"time", query.getText(6)}, {"source_session", std::to_string(static_cast<u64>(query.getInt64(5)))},
                    {"content", query.getText(4)}});
            }
        }
        for (const auto qq: senders) {
            if (const auto it = profiles.find(qq);
              it != profiles.end() &&
              (!it->second["stable_memory"].empty() || !it->second["recent_interactions"].empty() ||
                !it->second["pending_entries"].empty())) {
                result.push_back(std::move(it->second));
            }
        }
        return result;
    }
} // namespace insoulforge::PersonMemoryStore
