/// @file HttpTrace.cpp
/// @brief HTTP 请求完整内容内存缓存 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <chrono>
#include <infrastructure/http/HttpTrace.hpp>
#include <iomanip>
#include <ranges>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxEntries = 50;
        constexpr size_t kMaxBodySize = 1024 * 1024; // 单条请求/响应体上限

        std::string capBody(std::string body) {
            if (body.size() > kMaxBodySize) {
                body.resize(kMaxBodySize);
                body += "…(超出上限截断)";
            }
            return body;
        }

        std::string formatNow() {
            const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm localTime{};
#ifdef _WIN32
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif
            std::ostringstream stream;
            stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
            return stream.str();
        }
    } // namespace

    HttpTrace &HttpTrace::instance() {
        static HttpTrace trace;
        return trace;
    }

    void HttpTrace::append(HttpTraceEntry entry) {
        entry.timestamp = formatNow();
        entry.requestBody = capBody(std::move(entry.requestBody));
        entry.responseBody = capBody(std::move(entry.responseBody));

        std::lock_guard lock(m_mutex);
        entry.id = m_nextId++;
        if (m_entries.size() >= kMaxEntries) {
            m_entries.erase(m_entries.begin());
        }
        m_entries.push_back(std::move(entry));
    }

    std::vector<HttpTraceEntry> HttpTrace::query(const u64 afterId, const size_t limit) const {
        std::lock_guard lock(m_mutex);
        // id 按插入序递增，倒序遍历到边界即可停止；返回新的在前
        std::vector<HttpTraceEntry> result;
        for (const auto &entry: m_entries | std::views::reverse) {
            if (entry.id <= afterId)
                break;
            result.push_back(entry);
            if (result.size() >= limit)
                break;
        }
        return result;
    }

    size_t HttpTrace::size() const {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    void HttpTrace::clear() {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
    }
} // namespace insoulforge
