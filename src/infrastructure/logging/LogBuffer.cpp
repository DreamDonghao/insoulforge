/// @file LogBuffer.cpp
/// @brief 运行日志内存缓冲区与查询服务 - 实现

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <conversation/message/SessionId.hpp>
#include <regex>
#include <admin/realtime/LogWebSocketManager.hpp>
#include <spdlog/details/log_msg.h>
#include <infrastructure/logging/LogBuffer.hpp>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxEntries = 5000;
        const std::regex kSessionPatterns[] = {
          std::regex(R"((?:群|sessionId|group_id)[ =:：]+([0-9]{1,20}))", std::regex::icase),
          std::regex(R"(群([0-9]{1,20}))")};
        // 私聊日志前缀（Logger 对私聊会话输出 [private_id=QQ]），QQ 号需还原为带标志位的会话 ID
        const std::regex kPrivatePattern(R"(private_id[ =:：]+([0-9]{1,11}))");
    } // namespace

    LogBuffer &LogBuffer::instance() {
        static LogBuffer buffer;
        return buffer;
    }

    void LogBuffer::loadFromDirectory(const std::string &directory) {
        // 启动热路径：缓冲区只留最后 kMaxEntries 条。按新→旧读文件，每个文件只补足缺口，凑够即停，
        // 别把 6×10MB 滚动日志全量解析一遍拖慢启动
        std::vector<LogEntry> loaded;
        for (int index = 0; index <= 5 && loaded.size() < kMaxEntries; ++index) {
            // spdlog 滚动文件保留扩展名：bot.log / bot.1.log / bot.2.log ...
            const auto path =
              std::filesystem::path(directory) / (index == 0 ? "bot.log" : fmt::format("bot.{}.log", index));
            std::ifstream file(path);
            if (!file) {
                continue;
            }

            // 文件内是旧→新序；老文件只取最新的缺口条数，接到已收集内容（更新）之前
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
            // 会话 ID 靠正则提取，只对最终保留的条目做，加载阶段不付这笔开销
            entry.sessionId = extractSessionId(entry.message);
            entry.id = m_nextId++;
            m_entries.push_back(std::move(entry));
        }
    }

    void LogBuffer::append(const spdlog::details::log_msg &message) {
        LogEntry entry;
        entry.timestamp = formatTimestamp(message.time);
        const auto level = spdlog::level::to_string_view(message.level);
        entry.level = std::string(level.data(), level.size());
        if (entry.level == "warning")
            entry.level = "warn";
        if (entry.level == "err")
            entry.level = "error";
        entry.message = std::string(message.payload.data(), message.payload.size());
        entry.sessionId = extractSessionId(entry.message);

        json evt;
        {
            std::lock_guard lock(m_mutex);
            entry.id = m_nextId++;
            if (m_entries.size() >= kMaxEntries) {
                m_entries.erase(m_entries.begin());
            }
            evt["id"] = entry.id;
            evt["timestamp"] = entry.timestamp;
            evt["level"] = entry.level;
            evt["message"] = entry.message;
            // 会话 ID 可能带私聊标志位（超过 JS 安全整数范围），序列化为字符串
            evt["groupId"] = entry.sessionId.has_value() ? json(std::to_string(*entry.sessionId)) : json();
            m_entries.push_back(entry);
        }
        LogWebSocketManager::instance().pushLog(evt);
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
            if (end > query.limit) {
                start = end - query.limit;
            }
        } else {
            if (end > query.limit) {
                start = end - query.limit;
            }
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
        // 固定格式 [YYYY-MM-DD HH:MM:SS.mmm] [level] message（Logger 文件 sink 的 pattern），
        // 手工解析比 std::regex 快约两个数量级，这里是启动加载热路径
        constexpr size_t kStampWidth = 23; // 2026-09-04 22:21:00.123
        if (line.size() < kStampWidth + 5 || line[0] != '[' || line[kStampWidth + 1] != ']' ||
            line[kStampWidth + 2] != ' ' || line[kStampWidth + 3] != '[') {
            return std::nullopt;
        }

        const size_t levelEnd = line.find(']', kStampWidth + 4);
        if (levelEnd == std::string::npos || levelEnd + 2 > line.size() || line[levelEnd + 1] != ' ') {
            return std::nullopt;
        }

        LogEntry entry;
        entry.timestamp = line.substr(1, kStampWidth);
        entry.level = line.substr(kStampWidth + 4, levelEnd - (kStampWidth + 4));
        // 与 append 的等级归一化保持一致，否则重启后历史日志对不上前端的级别过滤
        if (entry.level == "warning")
            entry.level = "warn";
        if (entry.level == "err")
            entry.level = "error";
        entry.message = line.substr(levelEnd + 2);
        return entry;
    }

    std::optional<uint64_t> LogBuffer::extractSessionId(const std::string &message) {
        std::smatch match;
        if (std::regex_search(message, match, kPrivatePattern)) {
            try {
                return SessionId::fromPrivateUser(std::stoull(match[1].str()));
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }
        for (const auto &pattern: kSessionPatterns) {
            if (std::regex_search(message, match, pattern)) {
                try {
                    return std::stoull(match[1].str());
                } catch (const std::exception &) {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }

    std::string LogBuffer::formatTimestamp(const spdlog::log_clock::time_point &timestamp) {
        const auto time = spdlog::log_clock::to_time_t(timestamp);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        const auto milliseconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count() % 1000;
        return fmt::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", localTime, milliseconds);
    }

    bool LogBuffer::matches(const LogEntry &entry, const LogQuery &query) {
        if (query.systemOnly && entry.sessionId.has_value()) {
            return false;
        }
        if (query.sessionId.has_value() && entry.sessionId != query.sessionId) {
            return false;
        }
        if (query.level.has_value() && entry.level != *query.level) {
            return false;
        }
        return query.keyword.empty() || entry.message.find(query.keyword) != std::string::npos;
    }
} // namespace insoulforge
