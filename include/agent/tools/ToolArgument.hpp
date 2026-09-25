/// @file ToolArgument.hpp
/// @brief 工具调用参数的宽容读取辅助函数

#pragma once

#include <infrastructure/JsonUtil.hpp>
#include <string>

namespace insoulforge {
    /// @brief 从工具调用参数中读取字符串
    /// @param args 工具调用参数
    /// @param key 参数名称
    /// @return 参数值；缺失或类型不匹配时返回空字符串
    inline auto argString(const json &args, const char *key) -> std::string { return getStr(args, key); }
} // namespace insoulforge
