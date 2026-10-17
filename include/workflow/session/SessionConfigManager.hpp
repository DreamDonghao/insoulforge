/// @file SessionConfigManager.hpp
/// @brief 会话消息统计与显示名称管理
/// @details 为群聊和私聊提供消息计数访问与名称刷新，数据由 SessionStore 保存。

#pragma once

#include <string>

#include <drogon/utils/coroutine.h>

#include <infrastructure/NumericTypes.hpp>
#include <workflow/session/SessionStore.hpp>

/// @brief 通过 SessionStore 访问会话统计并刷新显示名称
namespace insoulforge::SessionConfigManager {
    /// @brief 获取会话统计
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @return 已保存的统计信息；不存在时消息数为 0
    [[nodiscard]] auto getConfig(u64 sessionId) -> SessionConfig;

    /// @brief 检查会话统计记录是否存在
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @return 是否存在
    [[nodiscard]] auto contains(u64 sessionId) -> bool;

    /// @brief 保存会话统计
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @param config 待保存的统计信息
    void addConfig(u64 sessionId, const SessionConfig &config = SessionConfig());

    /// @brief 增加消息计数
    /// @param sessionId 会话 ID（私聊会话带标志位）
    void incrementMessageCount(u64 sessionId);

    /// @brief 查询 OneBot 并更新会话名称（群聊为群名，私聊为 QQ 昵称）
    /// @param sessionId 会话 ID（私聊带标志位）
    /// @return 查询到的名称；为空时不覆盖原名称
    [[nodiscard]] auto fetchAndUpdateSessionName(u64 sessionId) -> drogon::Task<std::string>;
} // namespace insoulforge::SessionConfigManager
