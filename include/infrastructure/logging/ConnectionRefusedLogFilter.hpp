/// @file ConnectionRefusedLogFilter.hpp
/// @brief 仅限制 Trantor Connector 的重复连接拒绝警告，其他日志原样放行。
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>

namespace insoulforge {
    class ConnectionRefusedLogFilter {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto kReportInterval = std::chrono::minutes(5);

        [[nodiscard]] auto shouldWrite(std::string_view line, Clock::time_point now = Clock::now()) -> bool;
        void reset();

    private:
        std::mutex m_mutex;
        std::optional<Clock::time_point> m_lastReport;
    };
} // namespace insoulforge
