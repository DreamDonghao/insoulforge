/// @file MessageFormatter.hpp
/// @brief OneBot 消息文本的 CQ 码转换

#pragma once

#include <string>

#include <expected>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MessageFormatter {
    /// @brief 将归一化收藏转换为 CQ 消息；完整商城字段优先，否则使用图片 URL。
    [[nodiscard]] auto favoriteEmojiToCQCode(const json &emoji) -> std::expected<std::string, std::string>;
    /// @brief 将 @昵称 或 @[QQ:xxx] 转换为 [CQ:at,qq=xxx]，昵称从运行时目录查询
    [[nodiscard]] auto convertAtToCQCode(std::string text) -> std::string;
} // namespace insoulforge::MessageFormatter
