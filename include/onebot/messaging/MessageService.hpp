/// @file MessageService.hpp
/// @brief QQ 消息发送与会话名称查询
/// @details 通过 OneBotClient 发送群聊和私聊消息；发送成功后将助手消息写入工作流。

#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <infrastructure/NumericTypes.hpp>
#include <optional>
#include <string>

/// @brief 封装消息发送、@转换和会话名称更新
namespace insoulforge::MessageService {
    /// @brief 将文本中的@格式转换为 CQ 码
    /// @param text 原始文本（可能包含 @昵称 或 @[QQ:xxx] 格式）
    /// @return 转换后的文本（包含 [CQ:at,qq=xxx] 格式）
    auto convertAtToCQCode(std::string text) -> std::string;

    /// @brief 发送群消息
    /// @param groupId 群号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回 nullopt（已记日志）
    auto sendGroupMsg(u64 groupId, std::string message) -> drogon::Task<std::optional<u64>>;

    /// @brief 发送私聊消息
    /// @param userId 用户 QQ 号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回 nullopt（已记日志）
    auto sendPrivateMsg(u64 userId, std::string message) -> drogon::Task<std::optional<u64>>;

    /// @brief 获取并更新会话名称（群聊为群名，私聊为 QQ 昵称）
    /// @param sessionId 会话 ID（私聊带标志位）
    /// @return 会话名称
    [[nodiscard]] auto fetchAndUpdateSessionName(u64 sessionId) -> drogon::Task<std::string>;
} // namespace insoulforge::MessageService
