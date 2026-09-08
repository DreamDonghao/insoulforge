/// @file CommonUtil.hpp
/// @brief 通用工具函数
/// @author donghao
/// @date 2026-04-02
/// @details 提供通用工具函数：
///          - 无符号整数解析：parseUInt64()
///          - 时间获取与格式化：currentDateTime() / formatUnixTime() / formatTimeOfDay()
///          - 文本修剪：trim()
///          （JSON 相关工具见 util/JsonUtil.hpp）

#pragma once
#include <cctype>
#include <charconv>
#include <ctime>
#include <drogon/drogon.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>

/// @brief 去除字符串首尾空白（isspace 语义）
[[nodiscard]] inline std::string trim(std::string s) {
    const auto isSpace = [](const unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

/// @brief 尝试解析无符号整数（非抛出，替代 std::stoull）
/// @param s 输入字符串
/// @return 解析结果；要求整串都是数字（允许前导空白），否则返回 nullopt
[[nodiscard]] inline std::optional<uint64_t> tryParseUInt64(std::string_view s) {
    uint64_t value = 0;
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
/// @return 解析结果；要求整串都是数字，否则返回 fallback
[[nodiscard]] inline uint64_t parseUInt64(std::string_view s, uint64_t fallback = 0) {
    return tryParseUInt64(s).value_or(fallback);
}

#include <chrono>
#include <fmt/chrono.h>

/// @brief time_t 转本地 std::tm（各平台的安全转换）
inline std::tm localTime(const std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

inline std::string currentDateTime() {
    using namespace std::chrono;
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(system_clock::to_time_t(system_clock::now())));
}

/// @brief unix 秒格式化为本地时间 YYYY-MM-DD HH:MM:SS
[[nodiscard]] inline std::string formatUnixTime(const int64_t unixSec) {
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", localTime(static_cast<std::time_t>(unixSec)));
}

/// @brief unix 秒格式化为本地时间 HH:MM
[[nodiscard]] inline std::string formatTimeOfDay(const int64_t unixSec) {
    return fmt::format("{:%H:%M}", localTime(static_cast<std::time_t>(unixSec)));
}
