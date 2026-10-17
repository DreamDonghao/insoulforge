/// @file CommonUtil.hpp
/// @brief 通用工具函数
/// @details 提供通用工具函数：
///          - 无符号整数解析：parseUInt64()
///          - 时间获取与格式化：currentDateTime() / formatUnixTime() / formatTimeOfDay()
///          - 文本修剪：trim()
///          JSON 相关工具见 infrastructure/JsonUtil.hpp。

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <infrastructure/NumericTypes.hpp>

/// @brief 去除字符串首尾空白（isspace 语义）
[[nodiscard]] auto trim(std::string s) -> std::string;

/// @brief 尝试解析无符号整数（非抛出，替代 std::stoull）
/// @param s 输入字符串
/// @return 解析结果；允许前导空格或制表符，其余字符必须为数字
[[nodiscard]] auto tryParseUInt64(std::string_view s) -> std::optional<insoulforge::u64>;

/// @brief 解析无符号整数（非抛出，替代 std::stoull）
/// @param s 输入字符串
/// @param fallback 解析失败时返回的值
/// @return 解析结果；允许前导空格或制表符，解析失败时返回 fallback
[[nodiscard]] auto parseUInt64(std::string_view s, insoulforge::u64 fallback = 0) -> insoulforge::u64;

/// @brief time_t 转本地 std::tm（各平台的安全转换）
auto localTime(std::time_t t) -> std::tm;

auto currentDateTime() -> std::string;

/// @brief unix 秒格式化为本地时间 YYYY-MM-DD HH:MM:SS
[[nodiscard]] auto formatUnixTime(insoulforge::i64 unixSec) -> std::string;

/// @brief unix 秒格式化为本地时间 HH:MM
[[nodiscard]] auto formatTimeOfDay(insoulforge::i64 unixSec) -> std::string;
