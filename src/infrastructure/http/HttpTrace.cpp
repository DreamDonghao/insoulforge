/// @file HttpTrace.cpp
/// @brief HTTP 请求完整内容内存缓存 - 实现

#include <chrono>
#include <iomanip>
#include <ranges>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/http/HttpTrace.hpp>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxEntries = 50;
        auto formatNow() -> std::string {
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

    auto HttpTrace::instance() -> HttpTrace & {
        static HttpTrace trace;
        return trace;
    }

    void HttpTrace::append(HttpTraceEntry entry) {
        entry.timestamp = formatNow();
        std::lock_guard lock(m_mutex);
        entry.id = m_nextId++;
        if (m_entries.size() >= kMaxEntries) {
            m_entries.erase(m_entries.begin());
        }
        m_entries.push_back(std::move(entry));
    }

    auto HttpTrace::query(const u64 afterId, const size_t limit) const -> std::vector<HttpTraceEntry> {
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

    auto HttpTrace::find(const u64 id) const -> std::optional<HttpTraceEntry> {
        std::lock_guard lock(m_mutex);
        for (const auto &entry: m_entries | std::views::reverse) {
            if (entry.id == id)
                return entry;
            if (entry.id < id)
                break;
        }
        return std::nullopt;
    }

    auto HttpTrace::size() const -> size_t {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    void HttpTrace::clear() {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
    }
} // namespace insoulforge
