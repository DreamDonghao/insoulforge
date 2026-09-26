/// @file SessionConfigManager.hpp
/// @brief 会话消息统计的读写接口
/// @details 为群聊和私聊统一提供消息计数访问；启用状态与显示名称由 SessionStore 管理。

#pragma once

#include <conversation/session/SessionStore.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 通过 SessionStore 访问会话的累计消息数
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
} // namespace insoulforge::SessionConfigManager
