/// @file MemoryStore.cpp
/// @brief 会话外号与历史内容存储 - 实现

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge::MemoryStore {
    auto normalizeSessionMemory(const json &value) -> std::optional<json> {
        if (!value.is_object() || !atOrNull(value, "aliases").is_array() ||
            (value.contains("recent_events") && !value["recent_events"].is_array()) ||
            (value.contains("legacy_entries") && !value["legacy_entries"].is_array()))
            return std::nullopt;
        json result = {{"aliases", json::array()}};
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
            if (!value.contains(key))
                continue;
            result[key] = json::array();
            std::unordered_set<std::string> seen;
            for (const auto &entry: atOrNull(value, key)) {
                if (!entry.is_string())
                    return std::nullopt;
                auto text = trim(entry.get<std::string>());
                if (!text.empty() && seen.insert(text).second)
                    result[key].push_back(std::move(text));
            }
        }
        if (value.contains("conversation_summaries")) {
            const auto &summaries = value["conversation_summaries"];
            if (!summaries.is_array())
                return std::nullopt;
            for (const auto &summary: summaries) {
                if (!summary.is_object() || !atOrNull(summary, "start_time").is_string() ||
                    !atOrNull(summary, "end_time").is_string() || !atOrNull(summary, "summary").is_string() ||
                    trim(summary["summary"].get_ref<const std::string &>()).empty())
                    return std::nullopt;
            }
            result["conversation_summaries"] = summaries;
        }
        return result;
    }

    auto conversationSummaries(const std::string &memory) -> json {
        json parsed;
        if (!tryParseJson(memory, parsed))
            return json::array();
        const auto normalized = normalizeSessionMemory(parsed);
        if (!normalized || !normalized->contains("conversation_summaries"))
            return json::array();
        auto summaries = (*normalized)["conversation_summaries"];
        std::stable_sort(summaries.begin(), summaries.end(),
          [](const json &left, const json &right) { return getStr(left, "start_time") < getStr(right, "start_time"); });
        const auto limit =
          static_cast<size_t>(Config::instance().conversationSummaryMaxCount.load(std::memory_order_relaxed));
        if (summaries.size() > limit)
            summaries.erase(summaries.begin(), summaries.end() - static_cast<json::difference_type>(limit));
        return summaries;
    }

    auto projectShortTermMemory(const std::string &memory) -> json {
        json parsed;
        if (tryParseJson(memory, parsed)) {
            if (auto structured = normalizeSessionMemory(parsed))
                return {{"aliases", std::move((*structured)["aliases"])}};
        }
        return {{"aliases", json::array()}};
    }

    auto pendingLegacyEntries(const std::string &memory) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::unordered_set<std::string> seen;
        const auto append = [&](const std::string &text) {
            auto value = trim(text);
            if (!value.empty() && seen.insert(value).second)
                lines.push_back(std::move(value));
        };
        json parsed;
        if (tryParseJson(memory, parsed)) {
            if (auto structured = normalizeSessionMemory(parsed)) {
                for (const auto *key: {"recent_events", "legacy_entries"})
                    for (const auto &entry: atOrNull(*structured, key))
                        append(entry.get<std::string>());
                return lines;
            }
            if (parsed.is_array() && std::ranges::all_of(parsed, [](const json &entry) { return entry.is_string(); })) {
                for (const auto &entry: parsed)
                    append(entry.get<std::string>());
                return lines;
            }
        }
        for (size_t start = 0; start < memory.size();) {
            const auto end = memory.find('\n', start);
            const auto stop = end == std::string::npos ? memory.size() : end;
            append(memory.substr(start, stop - start));
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
