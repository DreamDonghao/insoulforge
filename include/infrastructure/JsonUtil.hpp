/// @file JsonUtil.hpp
/// @brief JSON 工具 - 基于 nlohmann/json 的统一封装
/// @details 项目统一 JSON 类型与工具函数：
///          - 类型别名：json = nlohmann::ordered_json（键按插入顺序保存与序列化）
///          - 解析：parseJson() / tryParseJson()
///          - 序列化：dumpJson()（紧凑）
///          - drogon 边界：parseJsonBody()（请求体）、jsonResponse()（响应），不经 JsonCpp
///          - 宽容取值（兼容 OneBot/LLM/前端输入的类型抖动，语义对齐旧 JsonCpp 的缺键返回 null）：
///            按键 getStr/getInt/getInt64/getUInt/getDouble/getBool，
///            按值 jsonToString/jsonToInt64/jsonToUInt64/jsonToDouble/jsonToBool，
///            嵌套安全取字段 atOrNull()

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    /// @brief 项目统一 JSON 类型：ordered_json 按插入顺序保存键（nlohmann::json 默认按字典序排序）
    using json = nlohmann::ordered_json;

    /// @brief 解析 JSON，失败时打印日志并返回 null
    [[nodiscard]] inline auto parseJson(const std::string &jsonStr) -> json {
        json parsed = json::parse(jsonStr, nullptr, false);
        if (parsed.is_discarded()) {
            Logger::warn(0, "Json", fmt::format("解析失败: {}", jsonStr.substr(0, 100)));
            return {};
        }
        return parsed;
    }

    /// @brief 尝试解析 JSON（非抛出、不打印日志）
    /// @param jsonStr 输入字符串
    /// @param root 输出的 JSON 值；解析失败时被置为 discarded
    /// @return 是否解析成功
    [[nodiscard]] inline auto tryParseJson(const std::string_view jsonStr, json &root) -> bool {
        root = json::parse(jsonStr, nullptr, false);
        return !root.is_discarded();
    }

    /// @brief 序列化为紧凑 JSON（与聊天记录等既有存储格式一致）
    /// @param emitUtf8 是否原样输出非 ASCII 字符（false 时转义为 \\uXXXX）
    /// @param indent 缩进宽度（-1 表示紧凑输出）
    /// @details 无效 UTF-8 字节替换为 U+FFFD 而不是抛异常：日志/LLM/自定义工具输出可能
    ///          携带坏字节，序列化处于所有对外边界（HTTP/WS/存储），不允许因坏字节中断
    [[nodiscard]] inline auto dumpJson(const json &value, const bool emitUtf8 = true, const i32 indent = -1)
      -> std::string {
        return value.dump(indent, ' ', !emitUtf8, json::error_handler_t::replace);
    }

    /// @brief 容忍模型输出 ```json 围栏等杂质：截取首尾大括号之间的内容
    [[nodiscard]] inline auto tryExtractJsonObject(const std::string &text, std::string &payload) -> bool {
        const size_t start = text.find('{');
        const size_t end = text.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            return false;
        payload = text.substr(start, end - start + 1);
        return true;
    }

    /// @brief 安全取字段：非对象或键缺失时返回 null（等价 JsonCpp 宽容的 const operator[]，
    ///        避免 nlohmann const [] 缺键的未定义行为）
    [[nodiscard]] inline auto atOrNull(const json &value, const char *key) -> const json & {
        static const json kNull;
        if (!value.is_object())
            return kNull;
        if (const auto it = value.find(key); it != value.end())
            return it.value();
        return kNull;
    }

    // ==================== 按值宽容转换 ====================

    /// @brief 字符串/数字 → 字符串（数字按 JSON 文本输出，对齐 JsonCpp asString 的宽容语义）
    [[nodiscard]] inline auto jsonToString(const json &value, std::string fallback = {}) -> std::string {
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number())
            return value.dump();
        return fallback;
    }

    [[nodiscard]] inline auto jsonToInt(const json &value, const i32 fallback = 0) -> i32 {
        if (value.is_number_unsigned()) {
            const auto n = value.get<u64>();
            return n <= static_cast<u64>(INT32_MAX) ? static_cast<i32>(n) : fallback;
        }
        if (value.is_number_integer())
            return static_cast<i32>(value.get<i64>());
        if (value.is_number_float())
            return static_cast<i32>(value.get<f64>());
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            i32 n = 0;
            if (const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
              ec == std::errc{} && ptr == s.data() + s.size())
                return n;
        }
        return fallback;
    }

    [[nodiscard]] inline auto jsonToInt64(const json &value, const i64 fallback = 0) -> i64 {
        if (value.is_number_unsigned()) {
            const auto n = value.get<u64>();
            return n <= static_cast<u64>(INT64_MAX) ? static_cast<i64>(n) : fallback;
        }
        if (value.is_number_integer())
            return value.get<i64>();
        if (value.is_number_float())
            return static_cast<i64>(value.get<f64>());
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            i64 n = 0;
            if (const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
              ec == std::errc{} && ptr == s.data() + s.size())
                return n;
        }
        return fallback;
    }

    /// @brief 数值/字符串 → u64（字符串按十进制解析；缺失/空/null/负数/浮点一律返回 fallback）
    [[nodiscard]] inline auto jsonToUInt64(const json &value, const u64 fallback = 0) -> u64 {
        if (value.is_number_unsigned())
            return value.get<u64>();
        if (value.is_number_integer()) {
            const auto n = value.get<i64>();
            return n >= 0 ? static_cast<u64>(n) : fallback;
        }
        if (value.is_string())
            return parseUInt64(value.get<std::string>(), fallback);
        return fallback;
    }

    [[nodiscard]] inline auto jsonToDouble(const json &value, const f64 fallback = 0.0) -> f64 {
        if (value.is_number())
            return value.get<f64>();
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            char *end = nullptr;
            const f64 n = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && *end == '\0')
                return n;
        }
        return fallback;
    }

    [[nodiscard]] inline auto jsonToBool(const json &value, const bool fallback = false) -> bool {
        if (value.is_boolean())
            return value.get<bool>();
        if (value.is_number())
            return value.get<f64>() != 0.0;
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            if (s == "true" || s == "1")
                return true;
            if (s == "false" || s == "0")
                return false;
        }
        return fallback;
    }

    // ==================== 按键宽容取值（键缺失/类型不符/非对象时返回 fallback，等价 JsonCpp get） ====================

    [[nodiscard]] inline auto getStr(const json &value, const char *key, std::string fallback = {}) -> std::string {
        return jsonToString(atOrNull(value, key), std::move(fallback));
    }

    [[nodiscard]] inline auto getInt(const json &value, const char *key, const i32 fallback = 0) -> i32 {
        return jsonToInt(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline auto getInt64(const json &value, const char *key, const i64 fallback = 0) -> i64 {
        return jsonToInt64(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline auto getUInt(const json &value, const char *key, const u64 fallback = 0) -> u64 {
        return jsonToUInt64(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline auto getDouble(const json &value, const char *key, const f64 fallback = 0.0) -> f64 {
        return jsonToDouble(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline auto getBool(const json &value, const char *key, const bool fallback = false) -> bool {
        return jsonToBool(atOrNull(value, key), fallback);
    }

    // ==================== drogon 边界 ====================

    /// @brief 解析请求体为 JSON 对象（等价 req->getJsonObject()，直接走 nlohmann 不经 JsonCpp）
    /// @return 请求体缺失、解析失败或不是对象时返回 nullopt
    [[nodiscard]] inline auto parseJsonBody(const drogon::HttpRequestPtr &req) -> std::optional<json> {
        if (!req)
            return std::nullopt;
        json parsed;
        if (!tryParseJson(req->body(), parsed) || !parsed.is_object())
            return std::nullopt;
        return parsed;
    }

    /// @brief 构造 JSON HTTP 响应（替代 HttpResponse::newHttpJsonResponse(Json::Value)）
    [[nodiscard]] inline auto jsonResponse(const json &value) -> drogon::HttpResponsePtr {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(dumpJson(value));
        return resp;
    }
} // namespace insoulforge
