/// @file SharedConversationContext.hpp
/// @brief 跨会话近期见闻，不包含可用于引用或操作消息的标识。

#pragma once

#include <optional>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::SharedConversationContext {
    /// @brief 投影已投递的聊天消息，所有段的可读内容共用 200 字符额度；内部执行记录不参与。
    [[nodiscard]] auto projectMessage(const json &record) -> std::optional<json>;

    /// @brief 读取其他参与共享会话的三天内近期消息；接收会话未参与共享时返回空数组。
    [[nodiscard]] auto build(u64 receiverId, i32 messagesPerSession) -> json;
} // namespace insoulforge::SharedConversationContext
