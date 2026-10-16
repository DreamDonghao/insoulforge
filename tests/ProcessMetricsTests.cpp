/// @file ProcessMetricsTests.cpp
/// @brief Resource parsing, retention and native sampling regression tests.

#include <iostream>
#include <stdexcept>

#include <sstream>

#include <infrastructure/monitoring/ProcessMetrics.hpp>

namespace {
    void require(const bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    }
} // namespace

auto main() -> int {
    using namespace insoulforge;
    try {
        std::istringstream status("Name: fixture\nVmRSS:\t 90468 kB\nVmHWM: 100000 kB\nThreads: 108\n");
        const auto parsed = parseProcessStatus(status);
        require(parsed.rssBytes == 90468ULL * 1024 && parsed.peakRssBytes == 100000ULL * 1024 && parsed.threads == 108,
          "Linux counters or KiB conversion incorrect");
        std::istringstream invalid("VmRSS: -1 kB\nVmHWM: 18446744073709551615 kB\nThreads: 3junk\n");
        const auto bad = parseProcessStatus(invalid);
        require(!bad.rssBytes && !bad.peakRssBytes && !bad.threads, "invalid values must be unavailable");
        std::istringstream missing("Name: fixture\n");
        require(!parseProcessStatus(missing).rssBytes, "missing RSS must not be zero");
        ProcessMetricsHistory history;
        require(history.snapshot(0).empty(), "new history must be empty");
        constexpr i64 start = ProcessMetricsHistory::kRetentionMilliseconds;
        for (size_t index = 0; index < 1500; ++index) {
            history.append({.timestamp = start + static_cast<i64>(index) * 60000, .rssBytes = static_cast<u64>(index)});
        }
        const auto last = start + 1499 * 60000;
        const auto retained = history.snapshot(last);
        require(retained.size() == 1440 && retained.front().rssBytes == 60 && retained.back().rssBytes == 1499,
          "ring buffer must evict oldest samples in order");
        require(history.snapshot(last + ProcessMetricsHistory::kRetentionMilliseconds).empty(),
          "24-hour history must expire even without new samples");
        history.append({.timestamp = last + 60000});
        const auto failed = history.snapshot(last + 60000);
        require(!failed.back().rssBytes && !failed.back().threads, "failed samples must remain gaps");
        ProcessMetricsHistory clockChange;
        clockChange.append({.timestamp = 2000, .rssBytes = 1});
        clockChange.append({.timestamp = 1000, .rssBytes = 2});
        const auto ordered = clockChange.snapshot(2000);
        require(ordered.front().timestamp == 1000 && ordered.back().timestamp == 2000,
          "clock adjustments must not reverse the chart");
        require(clockChange.snapshot(1500).size() == 1, "future timestamps must not enter the chart");
        const auto native = readProcessSample();
#if defined(__linux__) || defined(__APPLE__)
        require(native.rssBytes && *native.rssBytes > 0 && native.threads && *native.threads > 0,
          "native resource sampling failed");
        for (size_t index = 0; index < 100; ++index)
            require(readProcessSample().threads == native.threads, "sampling leaked thread resources");
#endif
        require(native.timestamp > 0, "sample timestamp unavailable");
        auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        require(loop != nullptr, "Drogon main event loop unavailable");
        auto &metrics = ProcessMetrics::instance();
        metrics.start(*loop);
        metrics.start(*loop);
        require(metrics.snapshot().size() == 1, "start must sample immediately and avoid duplicate timers");
        metrics.stop();
        metrics.stop();
        std::cout << "Process metrics tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
