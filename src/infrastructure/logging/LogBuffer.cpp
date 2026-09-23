/// @file LogBuffer.cpp
/// @brief 运行日志内存缓冲区与查询服务 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <infrastructure/logging/LogBuffer.hpp>
#include <ranges>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxEntries = 5000;
    }

    LogBuffer &LogBuffer::instance() {
        static LogBuffer buffer;
        return buffer;
    }

    void LogBuffer::loadFromDirectory(const std::string &directory) {
        std::vector<LogEntry> loaded;
        for (i32 index = 0; index <= 5 && loaded.size() < kMaxEntries; ++index) {
            const auto path =
              std::filesystem::path(directory) / (index == 0 ? "bot.log" : fmt::format("bot.{}.log", index));
            std::ifstream file(path);
            if (!file) {
                continue;
            }

            std::vector<LogEntry> fileEntries;
            std::string line;
            while (std::getline(file, line)) {
                if (auto entry = parseLine(line)) {
                    fileEntries.push_back(std::move(*entry));
                }
            }

            const size_t keep = std::min(fileEntries.size(), kMaxEntries - loaded.size());
            const auto first = static_cast<std::ptrdiff_t>(fileEntries.size() - keep);
            loaded.insert(loaded.begin(), std::make_move_iterator(fileEntries.begin() + first),
              std::make_move_iterator(fileEntries.end()));
        }

        std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_entries.reserve(loaded.size());
        for (auto &entry: loaded) {
            entry.id = m_nextId++;
            m_entries.push_back(std::move(entry));
        }
    }

    LogEntry LogBuffer::append(LogEntry entry) {
        std::lock_guard lock(m_mutex);
        entry.id = m_nextId++;
        if (m_entries.size() >= kMaxEntries) {
            m_entries.erase(m_entries.begin());
        }
        m_entries.push_back(entry);
        return entry;
    }

    LogQueryResult LogBuffer::query(const LogQuery &query) const {
        std::lock_guard lock(m_mutex);
        LogQueryResult result;
        if (!m_entries.empty()) {
            result.oldestId = m_entries.front().id;
            result.newestId = m_entries.back().id;
        }

        std::vector<const LogEntry *> matched;
        matched.reserve(m_entries.size());
        for (const auto &entry: m_entries) {
            if (matches(entry, query)) {
                matched.push_back(&entry);
            }
        }
        if (matched.empty()) {
            return result;
        }

        size_t start = 0;
        size_t end = matched.size();
        if (query.afterId > 0) {
            start = std::distance(matched.begin(),
              std::ranges::find_if(matched, [&](const auto *entry) { return entry->id > query.afterId; }));
            end = std::min(start + query.limit, matched.size());
        } else if (query.beforeId.has_value()) {
            end = std::distance(matched.begin(),
              std::ranges::find_if(matched, [&](const auto *entry) { return entry->id >= *query.beforeId; }));
            start = end > query.limit ? end - query.limit : 0;
        } else if (end > query.limit) {
            start = end - query.limit;
        }

        result.entries.reserve(end - start);
        for (size_t index = start; index < end; ++index) {
            result.entries.push_back(*matched[index]);
        }
        result.hasMore = start > 0 || end < matched.size();
        if (!result.entries.empty()) {
            result.nextBeforeId = result.entries.front().id;
            result.nextAfterId = result.entries.back().id;
        }
        return result;
    }

    size_t LogBuffer::size() const {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    std::optional<LogEntry> LogBuffer::parseLine(const std::string &line) {
        // 新文件格式：[YYYY-MM-DD HH:MM:SS.mmm] [level][session_id][source][content]。
        // 旧版文件额外携带 sink 的等级段：[timestamp] [level] content。
        constexpr size_t kTimestampWidth = 23;
        if (line.size() < kTimestampWidth + 8 || line.front() != '[' || line[kTimestampWidth + 1] != ']' ||
            line[kTimestampWidth + 2] != ' ') {
            return std::nullopt;
        }

        size_t cursor = kTimestampWidth + 3;
        const auto readPart = [&line, &cursor]() -> std::optional<std::string> {
            if (cursor >= line.size() || line[cursor] != '[') {
                return std::nullopt;
            }
            const size_t end = line.find(']', cursor + 1);
            if (end == std::string::npos) {
                return std::nullopt;
            }
            std::string value = line.substr(cursor + 1, end - cursor - 1);
            cursor = end + 1;
            return value;
        };

        const auto level = readPart();
        if (!level) {
            return std::nullopt;
        }
        std::string normalizedLevel = *level;
        if (normalizedLevel == "warning") {
            normalizedLevel = "warn";
        } else if (normalizedLevel == "err") {
            normalizedLevel = "error";
        }

        // 旧格式在等级之后以空格连接正文，不包含结构化字段。
        if (cursor >= line.size() || line[cursor] != '[') {
            const size_t contentStart = cursor < line.size() && line[cursor] == ' ' ? cursor + 1 : cursor;
            return LogEntry{.timestamp = line.substr(1, kTimestampWidth),
              .level = std::move(normalizedLevel),
              .source = "Legacy",
              .content = line.substr(contentStart)};
        }

        const auto sessionId = readPart();
        const auto source = readPart();

        if (!sessionId || !source) {
            return std::nullopt;
        }
        if (cursor + 1 >= line.size() || line[cursor] != '[' || line.back() != ']') {
            return std::nullopt;
        }

        try {
            return LogEntry{.timestamp = line.substr(1, kTimestampWidth),
              .level = std::move(normalizedLevel),
              .sessionId = std::stoull(*sessionId),
              .source = *source,
              .content = line.substr(cursor + 1, line.size() - cursor - 2)};
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    bool LogBuffer::matches(const LogEntry &entry, const LogQuery &query) {
        if (query.sessionId.has_value() && entry.sessionId != *query.sessionId) {
            return false;
        }
        if (query.level.has_value() && entry.level != *query.level) {
            return false;
        }
        return query.keyword.empty() || entry.source.find(query.keyword) != std::string::npos ||
               entry.content.find(query.keyword) != std::string::npos;
    }
} // namespace insoulforge
