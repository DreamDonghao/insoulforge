/// @file ProcessMetrics.cpp
/// @brief 读取操作系统进程计数，不启动外部命令或扫描内存映射。

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#include <infrastructure/monitoring/ProcessMetrics.hpp>

namespace insoulforge {
    namespace {
        auto nowMilliseconds() -> i64 {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
        }

        auto parseCounter(std::string_view value, const bool bytes) -> std::optional<u64> {
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string_view::npos)
                return std::nullopt;
            value.remove_prefix(first);
            u64 number = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{})
                return std::nullopt;
            auto suffix = std::string_view(end, static_cast<size_t>(value.data() + value.size() - end));
            const auto suffixStart = suffix.find_first_not_of(" \t\r");
            suffix = suffixStart == std::string_view::npos ? std::string_view{} : suffix.substr(suffixStart);
            if (bytes) {
                const auto last = suffix.find_last_not_of(" \t\r");
                if (last == std::string_view::npos || suffix.substr(0, last + 1) != "kB" ||
                    number > std::numeric_limits<u64>::max() / 1024)
                    return std::nullopt;
                return number * 1024;
            }
            return suffix.empty() ? std::optional{number} : std::nullopt;
        }
    } // namespace

    auto parseProcessStatus(std::istream &input) -> ProcessSample {
        ProcessSample result;
        for (std::string line; std::getline(input, line);) {
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const auto key = std::string_view(line).substr(0, colon);
            const auto value = std::string_view(line).substr(colon + 1);
            if (key == "VmRSS")
                result.rssBytes = parseCounter(value, true);
            else if (key == "VmHWM")
                result.peakRssBytes = parseCounter(value, true);
            else if (key == "Threads")
                result.threads = parseCounter(value, false);
        }
        return result;
    }

    auto readProcessSample() -> ProcessSample {
        ProcessSample result;
#if defined(__linux__)
        std::ifstream status("/proc/self/status");
        result = parseProcessStatus(status);
#elif defined(__APPLE__)
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
            KERN_SUCCESS)
            result.rssBytes = info.resident_size;
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
            result.peakRssBytes = static_cast<u64>(usage.ru_maxrss); // macOS 的单位是字节，而不是 KiB。
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t threadCount = 0;
        if (task_threads(mach_task_self(), &threads, &threadCount) == KERN_SUCCESS) {
            result.threads = threadCount;
            for (mach_msg_type_number_t index = 0; index < threadCount; ++index)
                mach_port_deallocate(mach_task_self(), threads[index]);
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
              static_cast<vm_size_t>(threadCount) * sizeof(thread_t));
        }
#endif
        result.timestamp = nowMilliseconds();
        return result;
    }

    void ProcessMetricsHistory::append(ProcessSample sample) {
        if (m_size == kCapacity) {
            m_samples[m_first] = std::move(sample);
            m_first = (m_first + 1) % kCapacity;
        } else {
            m_samples[(m_first + m_size) % kCapacity] = std::move(sample);
            ++m_size;
        }
    }

    auto ProcessMetricsHistory::snapshot(const i64 now) const -> std::vector<ProcessSample> {
        auto result = std::vector<ProcessSample>{};
        result.reserve(m_size);
        for (size_t index = 0; index < m_size; ++index) {
            const auto &entry = m_samples[(m_first + index) % kCapacity];
            if (entry.timestamp > now - kRetentionMilliseconds && entry.timestamp <= now)
                result.push_back(entry);
        }
        // 系统校时后仍按时间排列，避免曲线倒退或 latest 指向较早的记录。
        std::ranges::stable_sort(result, {}, &ProcessSample::timestamp);
        return result;
    }

    auto ProcessMetrics::instance() -> ProcessMetrics & {
        static ProcessMetrics metrics;
        return metrics;
    }

    void ProcessMetrics::start(trantor::EventLoop &loop) {
        if (m_loop)
            return;
        m_loop = &loop;
        sample();
        m_timer = loop.runEvery(kSampleIntervalSeconds, [this] { sample(); });
    }

    void ProcessMetrics::stop() {
        if (!m_loop)
            return;
        m_loop->invalidateTimer(m_timer);
        m_loop = nullptr;
    }

    void ProcessMetrics::sample() {
        auto entry = readProcessSample();
        std::lock_guard lock(m_mutex);
        m_history.append(std::move(entry));
    }

    auto ProcessMetrics::snapshot() const -> std::vector<ProcessSample> {
        std::lock_guard lock(m_mutex);
        return m_history.snapshot(nowMilliseconds());
    }
} // namespace insoulforge
