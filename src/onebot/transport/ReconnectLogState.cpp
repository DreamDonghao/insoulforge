/// @file ReconnectLogState.cpp
/// @brief 离线统计只影响日志，不改变连接尝试的频率。
#include <onebot/transport/ReconnectLogState.hpp>

namespace insoulforge {
    auto ReconnectLogState::record(const std::optional<i32> error, const Clock::time_point now)
      -> std::optional<Report> {
        const bool first = !m_offlineSince;
        if (first) m_offlineSince = now;
        if (error) {
            ++m_failedAttempts;
            m_lastError = error;
        }
        if (!first && now - m_lastReport < kReportInterval) return std::nullopt;
        m_lastReport = now;
        return Report{first, m_failedAttempts,
          std::chrono::duration_cast<std::chrono::seconds>(now - *m_offlineSince).count(), m_lastError};
    }

    auto ReconnectLogState::recover(const Clock::time_point now) -> std::optional<Report> {
        if (!m_offlineSince) return std::nullopt;
        const Report report{false, m_failedAttempts,
          std::chrono::duration_cast<std::chrono::seconds>(now - *m_offlineSince).count(), m_lastError};
        reset();
        return report;
    }

    void ReconnectLogState::reset() {
        m_offlineSince.reset();
        m_lastReport = {};
        m_failedAttempts = 0;
        m_lastError.reset();
    }
} // namespace insoulforge
