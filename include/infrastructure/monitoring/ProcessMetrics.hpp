/// @file ProcessMetrics.hpp
/// @brief 进程资源采样与固定容量的内存历史。

#pragma once

#include <array>
#include <istream>
#include <mutex>
#include <optional>
#include <vector>

#include <trantor/net/EventLoop.h>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    struct ProcessSample {
        i64 timestamp = 0; ///< 采样时的 Unix 毫秒时间戳。
        std::optional<u64> rssBytes;
        std::optional<u64> peakRssBytes; ///< 启动以来的 RSS 峰值，不是滚动窗口峰值。
        std::optional<u64> threads;
    };

    /// @brief 解析 Linux 状态文本；字段缺失或无效时保留空值。
    [[nodiscard]] auto parseProcessStatus(std::istream &input) -> ProcessSample;
    /// @brief 读取当前进程资源；平台不支持或读取失败的指标为空。
    [[nodiscard]] auto readProcessSample() -> ProcessSample;

    /// @brief 保存至多 1440 个采样点；并发访问由调用方同步。
    class ProcessMetricsHistory {
    public:
        static constexpr size_t kCapacity = 1440;
        static constexpr i64 kRetentionMilliseconds = 24LL * 60 * 60 * 1000;
        void append(ProcessSample sample);
        [[nodiscard]] auto snapshot(i64 now) const -> std::vector<ProcessSample>;

    private:
        std::array<ProcessSample, kCapacity> m_samples{};
        size_t m_first = 0;
        size_t m_size = 0;
    };

    /// @brief 使用应用主事件循环每分钟采样一次，不新增线程。
    /// @note start/stop 在主线程调用；HTTP 工作线程可以安全查询历史快照。
    class ProcessMetrics {
    public:
        static constexpr f64 kSampleIntervalSeconds = 60;
        static auto instance() -> ProcessMetrics &;
        void start(trantor::EventLoop &loop);
        void stop();
        [[nodiscard]] auto snapshot() const -> std::vector<ProcessSample>;

    private:
        void sample();
        mutable std::mutex m_mutex;
        ProcessMetricsHistory m_history;
        trantor::EventLoop *m_loop = nullptr; ///< 借用应用持有的事件循环，不负责释放。
        trantor::TimerId m_timer{};
    };
} // namespace insoulforge
