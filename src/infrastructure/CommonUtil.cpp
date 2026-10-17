/// @file CommonUtil.cpp
/// @brief 通用工具函数实现

#include <cctype>
#include <charconv>
#include <chrono>

#include <fmt/chrono.h>

#include <infrastructure/CommonUtil.hpp>

auto trim(std::string s) -> std::string {
    const auto isSpace = [](const unsigned char c) -> bool { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

auto tryParseUInt64(const std::string_view s) -> std::optional<insoulforge::u64> {
    insoulforge::u64 value = 0;
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    // 跳过前导空白（与 stoull 行为一致）
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    if (const auto [ptr, ec] = std::from_chars(begin, end, value); ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

auto parseUInt64(const std::string_view s, insoulforge::u64 fallback) -> insoulforge::u64 {
    return tryParseUInt64(s).value_or(fallback);
}

auto localTime(const std::time_t t) -> std::tm {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

auto currentDateTime() -> std::string {
    using namespace std::chrono;
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(system_clock::to_time_t(system_clock::now())));
}

auto formatUnixTime(const insoulforge::i64 unixSec) -> std::string {
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(unixSec));
}

auto formatTimeOfDay(const insoulforge::i64 unixSec) -> std::string {
    return fmt::format("{:%H:%M}", localTime(unixSec));
}
