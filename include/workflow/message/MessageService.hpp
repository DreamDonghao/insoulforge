/// @file MessageService.hpp
/// @brief 消息发送与发送结果上下文记录
/// @details 发送尝试均写入上下文；失败记录附带 send_error，不包含平台消息 ID。

#pragma once

#include <optional>
#include <string>

#include <drogon/utils/coroutine.h>
#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 组织消息发送，并将成功或失败结果写入工作流上下文
namespace insoulforge::MessageService {
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
} // namespace insoulforge::MessageService
