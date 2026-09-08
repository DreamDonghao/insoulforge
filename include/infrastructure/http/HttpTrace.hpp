/// @file HttpTrace.hpp
/// @brief HTTP 请求完整内容内存缓存（调试用）

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace insoulforge {
    struct HttpTraceEntry {
        uint64_t id = 0;
        std::string timestamp;
        std::string tag;
        std::string method;
        std::string url;
        std::string requestBody;
        int status = 0;
        std::string responseBody;
        std::optional<uint64_t> sessionId;
    };

    /// @brief 保存最近 N 条经过 HttpUtil 的完整请求/响应，供管理后台查询
    /// @details 仅存内存，重启清空；status 为 0 表示请求未得到响应（超时/异常）
    class HttpTrace {
    public:
        static HttpTrace &instance();

        void append(HttpTraceEntry entry);

        /// @brief 按 id 降序取记录（id > afterId，最多 limit 条）
        [[nodiscard]] std::vector<HttpTraceEntry> query(uint64_t afterId, size_t limit) const;

        [[nodiscard]] size_t size() const;

        void clear();

    private:
        HttpTrace() = default;

        mutable std::mutex m_mutex;
        std::vector<HttpTraceEntry> m_entries;
        uint64_t m_nextId = 1;
    };
} // namespace insoulforge
