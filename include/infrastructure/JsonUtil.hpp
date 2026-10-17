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

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 项目统一 JSON 类型：ordered_json 按插入顺序保存键（nlohmann::json 默认按字典序排序）
    using json = nlohmann::ordered_json;

    /// @brief 解析 JSON，失败时打印日志并返回 null
    [[nodiscard]] auto parseJson(const std::string &jsonStr) -> json;

    /// @brief 尝试解析 JSON（非抛出、不打印日志）
    /// @param jsonStr 输入字符串
    /// @param root 输出的 JSON 值；解析失败时被置为 discarded
    /// @return 是否解析成功
    [[nodiscard]] auto tryParseJson(std::string_view jsonStr, json &root) -> bool;

    /// @brief 序列化为紧凑 JSON（与聊天记录等既有存储格式一致）
    /// @param value
    /// @param emitUtf8 是否原样输出非 ASCII 字符（false 时转义为 \\uXXXX）
    /// @param indent 缩进宽度（-1 表示紧凑输出）
    /// @details 无效 UTF-8 字节替换为 U+FFFD 而不是抛异常：日志/LLM/自定义工具输出可能
    ///          携带坏字节，序列化处于所有对外边界（HTTP/WS/存储），不允许因坏字节中断
    [[nodiscard]] auto dumpJson(const json &value, bool emitUtf8 = true, i32 indent = -1) -> std::string;

    /// @brief 容忍模型输出 ```json 围栏等杂质：截取首尾大括号之间的内容
    [[nodiscard]] auto tryExtractJsonObject(const std::string &text, std::string &payload) -> bool;

    /// @brief 安全取字段：非对象或键缺失时返回 null（等价 JsonCpp 宽容的 const operator[]，
    ///        避免 nlohmann const [] 缺键的未定义行为）
    [[nodiscard]] auto atOrNull(const json &value, const char *key) -> const json &;

    // ==================== 按值宽容转换 ====================

    /// @brief 字符串/数字 → 字符串（数字按 JSON 文本输出，对齐 JsonCpp asString 的宽容语义）
    [[nodiscard]] auto jsonToString(const json &value, std::string fallback = {}) -> std::string;

    [[nodiscard]] auto jsonToInt(const json &value, i32 fallback = 0) -> i32;

    [[nodiscard]] auto jsonToInt64(const json &value, i64 fallback = 0) -> i64;

    /// @brief 数值/字符串 → u64（字符串按十进制解析；缺失/空/null/负数/浮点一律返回 fallback）
    [[nodiscard]] auto jsonToUInt64(const json &value, u64 fallback = 0) -> u64;

    [[nodiscard]] auto jsonToDouble(const json &value, f64 fallback = 0.0) -> f64;

    [[nodiscard]] auto jsonToBool(const json &value, bool fallback = false) -> bool;

    // ==================== 按键宽容取值（键缺失/类型不符/非对象时返回 fallback，等价 JsonCpp get） ====================

    [[nodiscard]] auto getStr(const json &value, const char *key, std::string fallback = {}) -> std::string;

    [[nodiscard]] auto getInt(const json &value, const char *key, i32 fallback = 0) -> i32;

    [[nodiscard]] auto getInt64(const json &value, const char *key, i64 fallback = 0) -> i64;

    [[nodiscard]] auto getUInt(const json &value, const char *key, u64 fallback = 0) -> u64;

    [[nodiscard]] auto getDouble(const json &value, const char *key, f64 fallback = 0.0) -> f64;

    [[nodiscard]] auto getBool(const json &value, const char *key, bool fallback = false) -> bool;

    // ==================== drogon 边界 ====================

    /// @brief 解析请求体为 JSON 对象（等价 req->getJsonObject()，直接走 nlohmann 不经 JsonCpp）
    /// @return 请求体缺失、解析失败或不是对象时返回 nullopt
    [[nodiscard]] auto parseJsonBody(const drogon::HttpRequestPtr &req) -> std::optional<json>;

    /// @brief 构造 JSON HTTP 响应（替代 HttpResponse::newHttpJsonResponse(Json::Value)）
    [[nodiscard]] auto jsonResponse(const json &value) -> drogon::HttpResponsePtr;
} // namespace insoulforge
