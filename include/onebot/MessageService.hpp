/// @file MessageService.hpp
/// @brief QQ 消息服务 - 消息发送与处理
/// @details 封装 QQ 消息的发送和处理逻辑（OneBot 协议交互见 OneBotClient）：
///          - 群消息/私聊消息发送（发送成功后由工作流记录并推送）
///          - @格式转换：convertAtToCQCode()
///          - 会话名称获取：fetchAndUpdateSessionName()

#pragma once
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

/// @brief 消息服务 - 封装 QQ 消息发送逻辑，对接 OneBot API
namespace insoulforge::MessageService {
    /// @brief 将文本中的@格式转换为 CQ 码
    /// @param text 原始文本（可能包含 @昵称 或 @[QQ:xxx] 格式）
    /// @return 转换后的文本（包含 [CQ:at,qq=xxx] 格式）
    std::string convertAtToCQCode(std::string text);

    /// @brief 发送群消息
    /// @param groupId 群号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回 nullopt（已记日志）
    drogon::Task<std::optional<uint64_t>> sendGroupMsg(uint64_t groupId, std::string message);

    /// @brief 发送私聊消息
    /// @param userId 用户QQ号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回 nullopt（已记日志）
    drogon::Task<std::optional<uint64_t>> sendPrivateMsg(uint64_t userId, std::string message);

    /// @brief 获取并更新会话名称（群聊为群名，私聊为 QQ 昵称）
    /// @param sessionId 会话 ID（私聊带标志位）
    /// @return 会话名称
    [[nodiscard]] drogon::Task<std::string> fetchAndUpdateSessionName(uint64_t sessionId);
} // namespace insoulforge::MessageService
