/// @file MessageFormatter.hpp
/// @brief OneBot 消息文本的 CQ 码转换

#pragma once

#include <string>

namespace insoulforge::MessageFormatter {
    /// @brief 将 @昵称 或 @[QQ:xxx] 转换为 [CQ:at,qq=xxx]，昵称从运行时目录查询
    [[nodiscard]] auto convertAtToCQCode(std::string text) -> std::string;
} // namespace insoulforge::MessageFormatter
