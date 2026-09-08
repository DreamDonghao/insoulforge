/// @file LogBuffer.hpp
/// @brief 运行日志内存缓冲区与查询服务

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <spdlog/details/log_msg.h>
#include <string>
#include <vector>

namespace insoulforge {
    struct LogEntry {
        uint64_t id = 0;
        std::string timestamp;
        std::string level;
        std::string message;
        std::optional<uint64_t> sessionId;
    };

    struct LogQuery {
        std::optional<uint64_t> sessionId;
        bool systemOnly = false;
        std::optional<std::string> level;
        std::string keyword;
        uint64_t afterId = 0;
        std::optional<uint64_t> beforeId;
        size_t limit = 200;
    };

    struct LogQueryResult {
        std::vector<LogEntry> entries;
        bool hasMore = false;
        uint64_t nextAfterId = 0;
        uint64_t nextBeforeId = 0;
        uint64_t oldestId = 0;
        uint64_t newestId = 0;
    };

    class LogBuffer {
    public:
        static LogBuffer &instance();

        void loadFromDirectory(const std::string &directory);

        void append(const spdlog::details::log_msg &message);

        [[nodiscard]] LogQueryResult query(const LogQuery &query) const;

        [[nodiscard]] size_t size() const;

    private:
        LogBuffer() = default;

        static std::optional<LogEntry> parseLine(const std::string &line);

        static std::optional<uint64_t> extractSessionId(const std::string &message);

        static std::string formatTimestamp(const spdlog::log_clock::time_point &timestamp);

        static bool matches(const LogEntry &entry, const LogQuery &query);

        mutable std::mutex m_mutex;
        std::vector<LogEntry> m_entries;
        uint64_t m_nextId = 1;
    };
}