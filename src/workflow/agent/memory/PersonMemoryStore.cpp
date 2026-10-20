/// @file PersonMemoryStore.cpp
/// @brief 全局人物记忆的定向读取。

#include <map>
#include <unordered_set>
#include <vector>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge::PersonMemoryStore {
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
            result.push_back({{"qq", std::to_string(static_cast<u64>(query.getInt64(0)))},
              {"affinity", query.getInt64(1)}, {"pending_count", query.getInt64(2)},
              {"updated_at", query.isNull(3) ? "" : query.getText(3)}});
        }
        return result;
    }

    auto getPerson(const u64 qq) -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement query(db.handle(),
          "SELECT p.stable_memory, p.recent_interactions, COALESCE(a.affinity, 0) "
          "FROM (SELECT qq_number FROM person_memories UNION SELECT qq_number FROM person_affinity) ids "
          "LEFT JOIN person_memories p ON p.qq_number = ids.qq_number "
          "LEFT JOIN person_affinity a ON a.qq_number = ids.qq_number WHERE ids.qq_number = ?");
        query.bind(1, qq);
        if (!query.step())
            return nullptr;
        json result = {{"qq", std::to_string(qq)}, {"affinity", query.getInt64(2)},
          {"stable_memory", query.isNull(0) ? json::array() : json::parse(query.getText(0))},
          {"recent_interactions", query.isNull(1) ? json::array() : json::parse(query.getText(1))},
          {"pending_entries", json::array()}};
        const Statement entries(db.handle(),
          "SELECT content, source_session, created_at FROM person_memory_entries WHERE qq_number = ? ORDER BY id DESC");
        entries.bind(1, qq);
        while (entries.step()) {
            result["pending_entries"].push_back({{"content", entries.getText(0)},
              {"source_session", std::to_string(static_cast<u64>(entries.getInt64(1)))}, {"time", entries.getText(2)}});
        }
        return result;
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
                it->second = {{"qq", std::to_string(qq)}, {"stable_memory", json::parse(query.getText(1))},
                  {"recent_interactions", json::parse(query.getText(2))}, {"pending_entries", json::array()}};
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
