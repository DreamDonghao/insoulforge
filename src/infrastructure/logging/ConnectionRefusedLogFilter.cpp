/// @file ConnectionRefusedLogFilter.cpp
/// @brief 使用平台 errno 识别连接拒绝，避免依赖本地化的错误描述和平台错误编号。
#include <cerrno>
#include <charconv>

#include <infrastructure/logging/ConnectionRefusedLogFilter.hpp>

namespace insoulforge {
    auto ConnectionRefusedLogFilter::shouldWrite(const std::string_view line, const Clock::time_point now) -> bool {
        constexpr std::string_view marker = "Connector::handleWrite - SO_ERROR = ";
        const auto pos = line.find(marker);
        if (pos == std::string_view::npos || !line.substr(0, pos).contains(" WARN ") ||
            !line.substr(pos).contains("Connector.cc:")) return true;
        const auto number = line.substr(pos + marker.size());
        int error = 0;
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), error);
        if (ec != std::errc{} || error != ECONNREFUSED || end == number.data() + number.size() || *end != ' ')
            return true;

        std::scoped_lock lock(m_mutex);
        if (m_lastReport && now - *m_lastReport < kReportInterval) return false;
        m_lastReport = now;
        return true;
    }

    void ConnectionRefusedLogFilter::reset() {
        std::scoped_lock lock(m_mutex);
        m_lastReport.reset();
    }
} // namespace insoulforge
