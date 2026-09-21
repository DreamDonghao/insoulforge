/// @file JsonUtil.hpp
/// @brief JSON 工具 - 基于 nlohmann/json 的统一封装
/// @author donghao
/// @date 2026-09-02
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
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <nlohmann/json.hpp>

namespace insoulforge {
    /// @brief 项目统一 JSON 类型：ordered_json 按插入顺序保存键（nlohmann::json 默认按字典序排序）
    using json = nlohmann::ordered_json;

    /// @brief 解析 JSON，失败时打印日志并返回 null
    [[nodiscard]] inline json parseJson(const std::string &jsonStr) {
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
    [[nodiscard]] inline bool tryParseJson(const std::string_view jsonStr, json &root) {
        root = json::parse(jsonStr, nullptr, false);
        return !root.is_discarded();
    }

    /// @brief 序列化为紧凑 JSON（与聊天记录等既有存储格式一致）
    /// @param emitUtf8 是否原样输出非 ASCII 字符（false 时转义为 \\uXXXX）
    /// @param indent 缩进宽度（-1 表示紧凑输出）
    /// @details 无效 UTF-8 字节替换为 U+FFFD 而不是抛异常：日志/LLM/自定义工具输出可能
    ///          携带坏字节，序列化处于所有对外边界（HTTP/WS/存储），不允许因坏字节中断
    [[nodiscard]] inline std::string dumpJson(const json &value, const bool emitUtf8 = true, const int indent = -1) {
        return value.dump(indent, ' ', !emitUtf8, json::error_handler_t::replace);
    }

    /// @brief 容忍模型输出 ```json 围栏等杂质：截取首尾大括号之间的内容
    [[nodiscard]] inline bool tryExtractJsonObject(const std::string &text, std::string &payload) {
        const size_t start = text.find('{');
        const size_t end = text.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            return false;
        payload = text.substr(start, end - start + 1);
        return true;
    }

    /// @brief 安全取字段：非对象或键缺失时返回 null（等价 JsonCpp 宽容的 const operator[]，
    ///        避免 nlohmann const [] 缺键的未定义行为）
    [[nodiscard]] inline const json &atOrNull(const json &value, const char *key) {
        static const json kNull;
        if (!value.is_object())
            return kNull;
        if (const auto it = value.find(key); it != value.end())
            return it.value();
        return kNull;
    }

    // ==================== 按值宽容转换 ====================

    /// @brief 字符串/数字 → 字符串（数字按 JSON 文本输出，对齐 JsonCpp asString 的宽容语义）
    [[nodiscard]] inline std::string jsonToString(const json &value, std::string fallback = {}) {
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number())
            return value.dump();
        return fallback;
    }

    [[nodiscard]] inline int jsonToInt(const json &value, const int fallback = 0) {
        if (value.is_number_unsigned()) {
            const auto n = value.get<uint64_t>();
            return n <= static_cast<uint64_t>(INT32_MAX) ? static_cast<int>(n) : fallback;
        }
        if (value.is_number_integer())
            return static_cast<int>(value.get<int64_t>());
        if (value.is_number_float())
            return static_cast<int>(value.get<double>());
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            int n = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
            if (ec == std::errc{} && ptr == s.data() + s.size())
                return n;
        }
        return fallback;
    }

    [[nodiscard]] inline int64_t jsonToInt64(const json &value, const int64_t fallback = 0) {
        if (value.is_number_unsigned()) {
            const auto n = value.get<uint64_t>();
            return n <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(n) : fallback;
        }
        if (value.is_number_integer())
            return value.get<int64_t>();
        if (value.is_number_float())
            return static_cast<int64_t>(value.get<double>());
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            int64_t n = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
            if (ec == std::errc{} && ptr == s.data() + s.size())
                return n;
        }
        return fallback;
    }

    /// @brief 数值/字符串 → uint64_t（字符串按十进制解析；缺失/空/null/负数/浮点一律返回 fallback）
    [[nodiscard]] inline uint64_t jsonToUInt64(const json &value, const uint64_t fallback = 0) {
        if (value.is_number_unsigned())
            return value.get<uint64_t>();
        if (value.is_number_integer()) {
            const auto n = value.get<int64_t>();
            return n >= 0 ? static_cast<uint64_t>(n) : fallback;
        }
        if (value.is_string())
            return parseUInt64(value.get<std::string>(), fallback);
        return fallback;
    }

    [[nodiscard]] inline double jsonToDouble(const json &value, const double fallback = 0.0) {
        if (value.is_number())
            return value.get<double>();
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            char *end = nullptr;
            const double n = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && *end == '\0')
                return n;
        }
        return fallback;
    }

    [[nodiscard]] inline bool jsonToBool(const json &value, const bool fallback = false) {
        if (value.is_boolean())
            return value.get<bool>();
        if (value.is_number())
            return value.get<double>() != 0.0;
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

    [[nodiscard]] inline std::string getStr(const json &value, const char *key, std::string fallback = {}) {
        return jsonToString(atOrNull(value, key), std::move(fallback));
    }

    [[nodiscard]] inline int getInt(const json &value, const char *key, const int fallback = 0) {
        return jsonToInt(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline int64_t getInt64(const json &value, const char *key, const int64_t fallback = 0) {
        return jsonToInt64(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline uint64_t getUInt(const json &value, const char *key, const uint64_t fallback = 0) {
        return jsonToUInt64(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline double getDouble(const json &value, const char *key, const double fallback = 0.0) {
        return jsonToDouble(atOrNull(value, key), fallback);
    }

    [[nodiscard]] inline bool getBool(const json &value, const char *key, const bool fallback = false) {
        return jsonToBool(atOrNull(value, key), fallback);
    }

    // ==================== drogon 边界 ====================

    /// @brief 解析请求体为 JSON 对象（等价 req->getJsonObject()，直接走 nlohmann 不经 JsonCpp）
    /// @return 请求体缺失、解析失败或不是对象时返回 nullopt
    [[nodiscard]] inline std::optional<json> parseJsonBody(const drogon::HttpRequestPtr &req) {
        if (!req)
            return std::nullopt;
        json parsed;
        if (!tryParseJson(req->body(), parsed) || !parsed.is_object())
            return std::nullopt;
        return parsed;
    }

    /// @brief 构造 JSON HTTP 响应（替代 HttpResponse::newHttpJsonResponse(Json::Value)）
    [[nodiscard]] inline drogon::HttpResponsePtr jsonResponse(const json &value) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(dumpJson(value));
        return resp;
    }
} // namespace insoulforge
