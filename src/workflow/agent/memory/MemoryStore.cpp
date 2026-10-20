/// @file MemoryStore.cpp
/// @brief 短期记忆存储 - 实现

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge::MemoryStore {
    auto normalizeSessionMemory(const json &value) -> std::optional<json> {
        if (!value.is_object() || !atOrNull(value, "aliases").is_array() ||
            !atOrNull(value, "recent_events").is_array() ||
            (value.contains("legacy_entries") && !value["legacy_entries"].is_array()))
            return std::nullopt;
        json result = {{"aliases", json::array()}, {"recent_events", json::array()}, {"legacy_entries", json::array()}};
        std::unordered_map<u64, size_t> aliasIndices;
        for (const auto &alias: value["aliases"]) {
            const auto qq = parseUInt64(getStr(alias, "qq"));
            if (!alias.is_object() || qq == 0 || qq == SessionId::kSystemAccountId || SessionId::isPrivate(qq) ||
                !atOrNull(alias, "names").is_array())
                return std::nullopt;
            auto [it, inserted] = aliasIndices.try_emplace(qq, result["aliases"].size());
            if (inserted)
                result["aliases"].push_back({{"qq", std::to_string(qq)}, {"names", json::array()}});
            auto &names = result["aliases"][it->second]["names"];
            for (const auto &name: alias["names"]) {
                if (!name.is_string())
                    return std::nullopt;
                auto text = trim(name.get<std::string>());
                if (!text.empty() && std::ranges::none_of(names, [&text](const json &existing) {
                        return existing.get_ref<const std::string &>() == text;
                    }))
                    names.push_back(std::move(text));
            }
        }
        for (const auto *key: {"recent_events", "legacy_entries"}) {
            std::unordered_set<std::string> seen;
            for (const auto &entry: atOrNull(value, key)) {
                if (!entry.is_string())
                    return std::nullopt;
                auto text = trim(entry.get<std::string>());
                if (!text.empty() && seen.insert(text).second)
                    result[key].push_back(std::move(text));
            }
        }
        return result;
    }

    auto projectShortTermMemory(const std::string &memory) -> json {
        json parsed;
        if (tryParseJson(memory, parsed)) {
            if (auto structured = normalizeSessionMemory(parsed))
                return std::move(*structured);
        }
        json lines = json::array();
        for (size_t start = 0; start < memory.size();) {
            const auto end = memory.find('\n', start);
            const auto stop = end == std::string::npos ? memory.size() : end;
            if (auto line = trim(memory.substr(start, stop - start)); !line.empty())
                lines.push_back(std::move(line));
            start = stop + 1;
        }
        return lines;
    }

    auto getShortTermMemory(const u64 sessionId) -> std::string {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT memory_content FROM short_term_memory WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step() ? stmt.getText(0) : "";
    }

    void updateShortTermMemory(const u64 sessionId, const std::string &memory) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT INTO short_term_memory (group_id, memory_content) VALUES (?, ?) "
                                          "ON CONFLICT(group_id) DO UPDATE SET memory_content = "
                                          "excluded.memory_content, updated_at = CURRENT_TIMESTAMP");
        stmt.bind(1, sessionId);
        stmt.bind(2, memory);
        stmt.exec();
    }

} // namespace insoulforge::MemoryStore
