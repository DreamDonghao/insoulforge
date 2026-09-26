/// @file CommonUtil.hpp
/// @brief 通用工具函数
/// @details 提供通用工具函数：
///          - 无符号整数解析：parseUInt64()
///          - 时间获取与格式化：currentDateTime() / formatUnixTime() / formatTimeOfDay()
///          - 文本修剪：trim()
///          JSON 相关工具见 infrastructure/JsonUtil.hpp。

#pragma once

#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

#include <drogon/drogon.h>

#include <infrastructure/NumericTypes.hpp>

/// @brief 去除字符串首尾空白（isspace 语义）
[[nodiscard]] inline auto trim(std::string s) -> std::string {
    const auto isSpace = [](const unsigned char c) -> bool { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

/// @brief 尝试解析无符号整数（非抛出，替代 std::stoull）
/// @param s 输入字符串
/// @return 解析结果；允许前导空格或制表符，其余字符必须为数字
[[nodiscard]] inline auto tryParseUInt64(std::string_view s) -> std::optional<insoulforge::u64> {
    insoulforge::u64 value = 0;
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    // 跳过前导空白（与 stoull 行为一致）
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

/// @brief 解析无符号整数（非抛出，替代 std::stoull）
/// @param s 输入字符串
/// @param fallback 解析失败时返回的值
/// @return 解析结果；允许前导空格或制表符，解析失败时返回 fallback
[[nodiscard]] inline auto parseUInt64(std::string_view s, insoulforge::u64 fallback = 0) -> insoulforge::u64 {
    return tryParseUInt64(s).value_or(fallback);
}

#include <chrono>
#include <fmt/chrono.h>

/// @brief time_t 转本地 std::tm（各平台的安全转换）
inline auto localTime(const std::time_t t) -> std::tm {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

inline auto currentDateTime() -> std::string {
    using namespace std::chrono;
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(system_clock::to_time_t(system_clock::now())));
}

/// @brief unix 秒格式化为本地时间 YYYY-MM-DD HH:MM:SS
[[nodiscard]] inline auto formatUnixTime(const insoulforge::i64 unixSec) -> std::string {
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(static_cast<std::time_t>(unixSec)));
}

/// @brief unix 秒格式化为本地时间 HH:MM
[[nodiscard]] inline auto formatTimeOfDay(const insoulforge::i64 unixSec) -> std::string {
    return fmt::format("{:%H:%M}", localTime(static_cast<std::time_t>(unixSec)));
}
