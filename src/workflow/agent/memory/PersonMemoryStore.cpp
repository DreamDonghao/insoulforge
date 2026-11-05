/// @file PersonMemoryStore.cpp
/// @brief 全局人物记忆的定向读取。

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/UserId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/Transaction.hpp>
#include <infrastructure/tokenizer/TokenCounter.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>

namespace insoulforge::PersonMemoryStore {
    namespace {
        // 调用方持有数据库锁，供读取及事务内快照校验共用。
        auto readPerson(const UserId &userId) -> json;
        // 查询结果中人物档案、关系和近期互动总是相邻的三列。
        auto readProfile(const UserId &userId, const Statement &row, const i32 memoryColumn = 0) -> json {
            return {{"user_id", userId.toString()},
              {"profile", row.isNull(memoryColumn) ? json::array() : json::parse(row.getText(memoryColumn))},
              {"relationship",
                row.isNull(memoryColumn + 1) ? json::array() : json::parse(row.getText(memoryColumn + 1))},
              {"recent_interactions",
                row.isNull(memoryColumn + 2) ? json::array() : json::parse(row.getText(memoryColumn + 2))},
              {"pending_entries", json::array()}};
        }

        // 详情和整理快照共用条目的列顺序：正文、来源会话、UTC 写入时间。
        auto readEntry(const Statement &row, const i32 contentColumn = 0) -> json {
            return {{"content", row.getText(contentColumn)}, {"source_session", row.getText(contentColumn + 1)},
              {"time", row.getText(contentColumn + 2)}};
        }
    } // namespace

    auto pendingPeople() -> std::vector<UserId> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(), "SELECT DISTINCT user_id FROM person_memory_entries ORDER BY user_id");
        std::vector<UserId> people;
        while (query.step()) {
            people.emplace_back(query.getText(0));
        }
        return people;
    }

    auto pendingSnapshot(const UserId &userId, const bool includeExisting) -> std::optional<PendingSnapshot> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement profile(
          db.handle(), "SELECT profile, relationship, recent_interactions FROM person_memories WHERE user_id = ?");
        profile.bind(1, userId.toString());
        if (!profile.step())
            return std::nullopt;
        PendingSnapshot snapshot{.userId = userId, .lastEntryId = 0, .profile = readProfile(userId, profile)};
        const Statement entries(db.handle(),
          "SELECT id, content, source_session, created_at FROM person_memory_entries WHERE user_id = ? ORDER BY id");
        entries.bind(1, userId.toString());
        while (entries.step()) {
            snapshot.lastEntryId = entries.getInt64(0);
            auto entry = readEntry(entries, 1);
            entry["id"] = std::to_string(snapshot.lastEntryId);
            snapshot.profile["pending_entries"].push_back(std::move(entry));
        }
        const bool hasMemory = !snapshot.profile["profile"].empty() || !snapshot.profile["relationship"].empty() ||
                               !snapshot.profile["recent_interactions"].empty();
        return snapshot.lastEntryId || (includeExisting && hasMemory) ? std::optional{std::move(snapshot)}
                                                                      : std::nullopt;
    }

    void applyConsolidation(const PendingSnapshot &snapshot, const json &profile) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const auto handle = db.handle();
        Transaction transaction(handle);
        const auto beforeTokens = contextSnapshot(snapshot.userId);
        const auto current = readPerson(snapshot.userId);
        json entries = json::array();
        if (!current.is_null()) {
            for (auto it = current["pending_entries"].rbegin(); it != current["pending_entries"].rend(); ++it)
                if (parseUInt64(getStr(*it, "id")) <= static_cast<u64>(snapshot.lastEntryId))
                    entries.push_back(*it);
        }
        if (current.is_null() || current["profile"] != snapshot.profile["profile"] ||
            current["relationship"] != snapshot.profile["relationship"] ||
            current["recent_interactions"] != snapshot.profile["recent_interactions"] ||
            entries != snapshot.profile["pending_entries"])
            throw ConsolidationConflict("人物记忆已修改，丢弃过期整理结果");
        const Statement update(handle,
          "UPDATE person_memories SET profile = ?, relationship = ?, recent_interactions = ? WHERE user_id = ?");
        update.bind(1, profile.at("profile").dump());
        update.bind(2, profile.at("relationship").dump());
        update.bind(3, profile.at("recent_interactions").dump());
        update.bind(4, snapshot.userId.toString());
        update.execOrThrow();
        if (Statement::changes(handle) != 1)
            throw ConsolidationConflict("人物档案不存在");
        const Statement remove(handle, "DELETE FROM person_memory_entries WHERE user_id = ? AND id <= ?");
        remove.bind(1, snapshot.userId.toString());
        remove.bind(2, snapshot.lastEntryId);
        remove.execOrThrow();
        updateTokenEstimate(snapshot.userId, beforeTokens);
        transaction.commit();
    }

    auto dailyState() -> DailyState {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(
          db.handle(), "SELECT completed_day, running_day FROM person_memory_daily_state WHERE id = 1");
        return query.step() ? DailyState{.completedDay = query.getText(0), .runningDay = query.getText(1)}
                            : DailyState{};
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
        const bool affinityEnabled = Config::instance().affinityEnabled.load(std::memory_order_relaxed);
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(),
          "WITH people AS (SELECT user_id FROM person_memories UNION SELECT user_id FROM person_affinity) "
          "SELECT p.user_id, COALESCE(a.affinity, 0), COUNT(e.id), MAX(e.created_at), COALESCE(m.estimated_tokens, 0) "
          "FROM people p LEFT JOIN person_memories m ON m.user_id = p.user_id LEFT JOIN person_affinity a ON a.user_id "
          "= p.user_id "
          "LEFT JOIN person_memory_entries e ON e.user_id = p.user_id "
          "GROUP BY p.user_id ORDER BY p.user_id");
        json result = json::array();
        while (query.step()) {
            result.push_back(
              {{"user_id", query.getText(0)}, {"affinity", query.getInt64(1)}, {"estimated_tokens", query.getInt64(4)},
                {"pending_count", query.getInt64(2)}, {"updated_at", query.isNull(3) ? "" : query.getText(3)}});
            if (!affinityEnabled)
                result.back().erase("affinity");
        }
        return result;
    }

    auto getPerson(const UserId &userId) -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        auto result = readPerson(userId);
        if (result.is_object() && !Config::instance().affinityEnabled.load(std::memory_order_relaxed))
            result.erase("affinity");
        return result;
    }

    namespace {
        auto readPerson(const UserId &userId) -> json {
            const auto &db = Database::instance();
            const Statement query(db.handle(),
              "SELECT p.profile, p.relationship, p.recent_interactions, COALESCE(a.affinity, 0) "
              "FROM (SELECT user_id FROM person_memories UNION SELECT user_id FROM person_affinity) ids "
              "LEFT JOIN person_memories p ON p.user_id = ids.user_id "
              "LEFT JOIN person_affinity a ON a.user_id = ids.user_id WHERE ids.user_id = ?");
            query.bind(1, userId.toString());
            if (!query.step())
                return nullptr;
            json result = {{"user_id", userId.toString()}, {"affinity", query.getInt64(3)}};
            result.update(readProfile(userId, query));
            const Statement tokens(db.handle(), "SELECT estimated_tokens FROM person_memories WHERE user_id = ?");
            tokens.bind(1, userId.toString());
            result["estimated_tokens"] = tokens.step() ? tokens.getInt64(0) : 0;
            const Statement entries(db.handle(), "SELECT content, source_session, created_at, id FROM "
                                                 "person_memory_entries WHERE user_id = ? ORDER BY id DESC");
            entries.bind(1, userId.toString());
            while (entries.step()) {
                auto entry = readEntry(entries);
                entry["id"] = std::to_string(entries.getInt64(3));
                result["pending_entries"].push_back(std::move(entry));
            }
            return result;
        }
    } // namespace

    namespace {
        auto matches(const json &current, const json &expected, const bool affinityEnabled) -> bool {
            if (current.is_null() || !expected.is_object())
                return false;
            for (const auto key:
              {"user_id", "affinity", "profile", "relationship", "recent_interactions", "pending_entries"}) {
                if (std::string_view(key) == "affinity" && !affinityEnabled) {
                    continue;
                }
                if (!expected.contains(key) || current.at(key) != expected.at(key)) {
                    return false;
                }
            }
            return true;
        }

        auto mutatePerson(const UserId &userId, const json &expected, const json *replacement) -> bool {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            auto *handle = db.handle();
            Transaction transaction(handle);
            const bool affinityEnabled = Config::instance().affinityEnabled.load(std::memory_order_relaxed);
            if (!matches(readPerson(userId), expected, affinityEnabled) ||
                (replacement && affinityEnabled && !atOrNull(*replacement, "affinity").is_number_integer())) {
                transaction.rollback();
                return false;
            }
            const auto beforeTokens = contextSnapshot(userId);
            if (replacement) {
                const Statement profile(handle,
                  "INSERT INTO person_memories (user_id, profile, relationship, recent_interactions) VALUES (?, "
                  "?, ?, ?) ON CONFLICT(user_id) DO UPDATE SET profile=excluded.profile, "
                  "relationship=excluded.relationship, "
                  "recent_interactions=excluded.recent_interactions");
                profile.bind(1, userId.toString());
                profile.bind(2, replacement->at("profile").dump());
                profile.bind(3, replacement->at("relationship").dump());
                profile.bind(4, replacement->at("recent_interactions").dump());
                profile.execOrThrow();
                if (affinityEnabled && Config::instance().affinityEnabled.load(std::memory_order_relaxed)) {
                    const Statement score(handle, "INSERT INTO person_affinity (user_id, affinity) VALUES (?, ?) ON "
                                                  "CONFLICT(user_id) DO UPDATE SET affinity=excluded.affinity");
                    score.bind(1, userId.toString());
                    score.bind(2, replacement->at("affinity").get<i32>());
                    score.execOrThrow();
                }
                std::unordered_map<std::string, std::string> contents;
                for (const auto &entry: replacement->at("pending_entries"))
                    contents.emplace(getStr(entry, "id"), getStr(entry, "content"));
                for (const auto &old: expected.at("pending_entries")) {
                    const auto id = getStr(old, "id");
                    const auto found = contents.find(id);
                    if (found != contents.end() && found->second == getStr(old, "content"))
                        continue;
                    const Statement change(
                      handle, found == contents.end()
                                ? "DELETE FROM person_memory_entries WHERE user_id=? AND id=?"
                                : "UPDATE person_memory_entries SET content=?3 WHERE user_id=?1 AND id=?2");
                    change.bind(1, userId.toString());
                    change.bind(2, parseUInt64(id));
                    if (found != contents.end())
                        change.bind(3, found->second);
                    change.execOrThrow();
                }
            } else {
                for (const auto table: {"person_memory_entries", "person_memories", "person_affinity"}) {
                    const Statement remove(handle, std::string("DELETE FROM ") + table + " WHERE user_id=?");
                    remove.bind(1, userId.toString());
                    remove.execOrThrow();
                }
            }
            if (replacement)
                updateTokenEstimate(userId, beforeTokens);
            transaction.commit();
            return true;
        }
    } // namespace

    auto updatePerson(const UserId &userId, const json &expected, const json &replacement) -> bool {
        return mutatePerson(userId, expected, &replacement);
    }
    auto deletePerson(const UserId &userId, const json &expected) -> bool {
        return mutatePerson(userId, expected, nullptr);
    }

    auto contextSnapshot(const UserId &userId) -> json {
        const auto person = readPerson(userId);
        if (person.is_null())
            return nullptr;
        json result = {{"profile", person["profile"]}, {"relationship", person["relationship"]},
          {"recent_interactions", person["recent_interactions"]}, {"pending_entries", json::array()}};
        for (auto it = person["pending_entries"].rbegin(); it != person["pending_entries"].rend(); ++it)
            result["pending_entries"].push_back({{"created_at", (*it)["time"]},
              {"source_session", (*it)["source_session"]}, {"content", (*it)["content"]}});
        if (result["profile"].empty() && result["relationship"].empty() &&
            result["recent_interactions"].empty() && result["pending_entries"].empty()) return nullptr;
        result["uid"] = userId.toString();
        return result;
    }

    void updateTokenEstimate(const UserId &userId, const json &before) {
        const auto after = contextSnapshot(userId);
        if (before == after) {
            return;
        }
        const auto estimate = after.is_null() ? std::expected<u32, std::string>{0}
                                             : countTokens(dumpJson(after));
        if (!estimate) {
            Logger::error(std::nullopt, "PersonMemory", estimate.error());
        }
        const Statement update(
          Database::instance().handle(), "UPDATE person_memories SET estimated_tokens = ? WHERE user_id = ?");
        update.bind(1, static_cast<i64>(estimate ? *estimate : 0));
        update.bind(2, userId.toString());
        update.execOrThrow();
    }

    auto getForSenders(const json &recentConversation) -> json {
        std::vector<UserId> senders;
        std::unordered_set<UserId> seen;
        for (const auto &record: recentConversation) {
            if (const auto userId = UserId::parse(getStr(atOrNull(record, "sender"), "user_id"));
              userId && userId->toString() != "self" && userId->toString() != "system" && seen.insert(*userId).second) {
                senders.push_back(*userId);
            }
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
        const Statement query(
          db.handle(), "SELECT p.user_id, p.profile, p.relationship, p.recent_interactions, "
                       "e.id, e.content, e.source_session, e.created_at "
                       "FROM person_memories p LEFT JOIN person_memory_entries e ON e.user_id = p.user_id "
                       "WHERE p.user_id IN (" +
                         placeholders + ") ORDER BY p.user_id, e.id");
        for (size_t index = 0; index < senders.size(); ++index)
            query.bind(static_cast<i32>(index + 1), senders[index].toString());
        std::map<UserId, json> profiles;
        while (query.step()) {
            const auto userId = UserId{query.getText(0)};
            auto [it, inserted] = profiles.try_emplace(userId);
            if (inserted) {
                it->second = readProfile(userId, query, 1);
            }
            if (!query.isNull(4)) {
                it->second["pending_entries"].push_back({{"created_at", query.getText(7)},
                  {"source_session", query.getText(6)}, {"content", query.getText(5)}});
            }
        }
        for (const auto &userId: senders) {
            if (const auto it = profiles.find(userId);
              it != profiles.end() &&
              (!it->second["profile"].empty() || !it->second["relationship"].empty() ||
                !it->second["recent_interactions"].empty() || !it->second["pending_entries"].empty())) {
                result.push_back(std::move(it->second));
            }
        }
        return result;
    }
} // namespace insoulforge::PersonMemoryStore
