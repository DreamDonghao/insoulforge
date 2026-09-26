/// @file HttpTrace.hpp
/// @brief HTTP 请求完整内容内存缓存（调试用）

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace insoulforge {
    struct HttpTraceEntry {
        u64 id = 0;
        std::string timestamp;
        std::string tag;
        std::string method;
        std::string url;
        std::string requestBody;
        i32 status = 0;
        std::string responseBody;
        std::optional<u64> sessionId;
    };

    /// @brief 保存最近 50 条经过 HttpUtil 的请求和响应，供后台调试
    /// @details 每条请求体和响应体最多保留 1 MiB；仅存内存，重启清空。status 为 0 表示未得到响应。
    class HttpTrace {
    public:
        static auto instance() -> HttpTrace &;

        void append(HttpTraceEntry entry);

        /// @brief 按 id 降序取记录（id > afterId，最多 limit 条）
        [[nodiscard]] auto query(u64 afterId, size_t limit) const -> std::vector<HttpTraceEntry>;

        [[nodiscard]] auto size() const -> size_t;

        void clear();

    private:
        HttpTrace() = default;

        mutable std::mutex m_mutex;
        std::vector<HttpTraceEntry> m_entries;
        u64 m_nextId = 1;
    };
} // namespace insoulforge
