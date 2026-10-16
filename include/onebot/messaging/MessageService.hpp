/// @file MessageService.hpp
/// @brief QQ 消息发送与会话名称查询
/// @details 发送尝试均写入上下文；失败记录附带 send_error，不包含平台消息 ID。

#pragma once

#include <expected>
#include <optional>
#include <string>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 封装消息发送、@转换和会话名称更新
namespace insoulforge::MessageService {
    /// @brief 将文本中的@格式转换为 CQ 码
    /// @param text 原始文本（可能包含 @昵称 或 @[QQ:xxx] 格式）
    /// @return 转换后的文本（包含 [CQ:at,qq=xxx] 格式）
    auto convertAtToCQCode(std::string text) -> std::string;

    /// @brief 发送群消息
    /// @param groupId 群号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回具体错误（同时写入上下文）。
    /// @param imageDescription 可选的图片描述，写入助手记录；发送状态由 send_error 区分。
    /// @param toolHistory 可选的执行历史，附在助手记录中，不发送给 OneBot。
    auto sendGroupMsg(u64 groupId, std::string message, std::optional<std::string> imageDescription = std::nullopt,
      json toolHistory = {}) -> drogon::Task<std::expected<u64, std::string>>;

    /// @brief 发送私聊消息
    /// @param userId 用户 QQ 号
    /// @param message 消息内容
    /// @return 发送成功返回 message_id，失败返回具体错误（同时写入上下文）。
    /// @param imageDescription 可选的图片描述，写入助手记录；发送状态由 send_error 区分。
    /// @param toolHistory 可选的执行历史，附在助手记录中，不发送给 OneBot。
    auto sendPrivateMsg(u64 userId, std::string message, std::optional<std::string> imageDescription = std::nullopt,
      json toolHistory = {}) -> drogon::Task<std::expected<u64, std::string>>;

    /// @brief 获取并更新会话名称（群聊为群名，私聊为 QQ 昵称）
    /// @param sessionId 会话 ID（私聊带标志位）
    /// @return 会话名称
    [[nodiscard]] auto fetchAndUpdateSessionName(u64 sessionId) -> drogon::Task<std::string>;
} // namespace insoulforge::MessageService
