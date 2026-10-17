/// @file JsonUtil.cpp
/// @brief JSON parsing, serialization, conversion and HTTP boundary helpers.

#include <charconv>
#include <cstdlib>
#include <utility>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    auto parseJson(const std::string &jsonStr) -> json {
        json parsed = json::parse(jsonStr, nullptr, false);
        if (parsed.is_discarded()) {
            Logger::warn(0, "Json", fmt::format("解析失败: {}", jsonStr.substr(0, 100)));
            return {};
        }
        return parsed;
    }

    auto tryParseJson(const std::string_view jsonStr, json &root) -> bool {
        root = json::parse(jsonStr, nullptr, false);
        return !root.is_discarded();
    }

    auto dumpJson(const json &value, const bool emitUtf8, const i32 indent) -> std::string {
        return value.dump(indent, ' ', !emitUtf8, json::error_handler_t::replace);
    }

    auto tryExtractJsonObject(const std::string &text, std::string &payload) -> bool {
        const size_t start = text.find('{');
        const size_t end = text.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            return false;
        payload = text.substr(start, end - start + 1);
        return true;
    }

    auto atOrNull(const json &value, const char *key) -> const json & {
        static const json kNull;
        if (!value.is_object())
            return kNull;
        if (const auto it = value.find(key); it != value.end())
            return it.value();
        return kNull;
    }

    auto jsonToString(const json &value, std::string fallback) -> std::string {
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number())
            return value.dump();
        return fallback;
    }

    auto jsonToInt(const json &value, const i32 fallback) -> i32 {
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

    auto jsonToInt64(const json &value, const i64 fallback) -> i64 {
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

    auto jsonToUInt64(const json &value, const u64 fallback) -> u64 {
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

    auto jsonToDouble(const json &value, const f64 fallback) -> f64 {
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

    auto jsonToBool(const json &value, const bool fallback) -> bool {
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

    auto getStr(const json &value, const char *key, std::string fallback) -> std::string {
        return jsonToString(atOrNull(value, key), std::move(fallback));
    }

    auto getInt(const json &value, const char *key, const i32 fallback) -> i32 {
        return jsonToInt(atOrNull(value, key), fallback);
    }

    auto getInt64(const json &value, const char *key, const i64 fallback) -> i64 {
        return jsonToInt64(atOrNull(value, key), fallback);
    }

    auto getUInt(const json &value, const char *key, const u64 fallback) -> u64 {
        return jsonToUInt64(atOrNull(value, key), fallback);
    }

    auto getDouble(const json &value, const char *key, const f64 fallback) -> f64 {
        return jsonToDouble(atOrNull(value, key), fallback);
    }

    auto getBool(const json &value, const char *key, const bool fallback) -> bool {
        return jsonToBool(atOrNull(value, key), fallback);
    }

    auto parseJsonBody(const drogon::HttpRequestPtr &req) -> std::optional<json> {
        if (!req)
            return std::nullopt;
        json parsed;
        if (!tryParseJson(req->body(), parsed) || !parsed.is_object())
            return std::nullopt;
        return parsed;
    }

    auto jsonResponse(const json &value) -> drogon::HttpResponsePtr {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(dumpJson(value));
        return resp;
    }
} // namespace insoulforge
