/// @file LogBuffer.hpp
/// @brief 运行日志内存缓冲区与查询服务

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    struct LogEntry {
        u64 id = 0;
        std::string timestamp;
        std::string level;
        u64 sessionId = 0;
        std::string source;
        std::string content;
    };

    struct LogQuery {
        std::optional<u64> sessionId;
        std::optional<std::string> level;
        std::string keyword;
        u64 afterId = 0;
        std::optional<u64> beforeId;
        size_t limit = 200;
    };

    struct LogQueryResult {
        std::vector<LogEntry> entries;
        bool hasMore = false;
        u64 nextAfterId = 0;
        u64 nextBeforeId = 0;
        u64 oldestId = 0;
        u64 newestId = 0;
    };

    class LogBuffer {
    public:
        static LogBuffer &instance();

        void loadFromDirectory(const std::string &directory);

        /// @brief 追加由 Logger 创建的日志条目
        /// @return 带内存序列 ID 的最终条目
        [[nodiscard]] LogEntry append(LogEntry entry);

        [[nodiscard]] LogQueryResult query(const LogQuery &query) const;

        [[nodiscard]] size_t size() const;

    private:
        LogBuffer() = default;

        static std::optional<LogEntry> parseLine(const std::string &line);

        static bool matches(const LogEntry &entry, const LogQuery &query);

        mutable std::mutex m_mutex;
        std::vector<LogEntry> m_entries;
        u64 m_nextId = 1;
    };
} // namespace insoulforge
