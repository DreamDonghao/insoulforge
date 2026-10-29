/// @file ReconnectLogState.hpp
/// @brief 单次离线期间的重试统计与日志节流；由连接管理器持锁访问。
#pragma once

#include <chrono>
#include <optional>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class ReconnectLogState {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto kReportInterval = std::chrono::minutes(5);

        struct Report {
            bool first;
            u64 failedAttempts;
            i64 offlineSeconds;
            std::optional<i32> lastError;
        };

        /// @brief 记录断开或连接失败；error 为空表示已建立的连接断开，不算连接尝试失败。
        [[nodiscard]] auto record(std::optional<i32> error, Clock::time_point now = Clock::now())
          -> std::optional<Report>;
        /// @brief 结束本次离线并清空统计；没有离线记录时返回空值。
        [[nodiscard]] auto recover(Clock::time_point now = Clock::now()) -> std::optional<Report>;
        void reset();

    private:
        std::optional<Clock::time_point> m_offlineSince;
        Clock::time_point m_lastReport{};
        u64 m_failedAttempts = 0;
        std::optional<i32> m_lastError;
    };
} // namespace insoulforge
