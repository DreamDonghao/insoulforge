/// @file SessionConfigManager.hpp
/// @brief 会话配置管理器 - 会话状态与统计管理
/// @author donghao
/// @date 2026-04-02
/// @details 管理会话（群聊/私聊）配置信息，使用 SQLite 持久化存储。
///          支持会话配置的增删改查和消息计数统计。

#pragma once

#include <conversation/session/SessionStore.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 会话配置管理
/// @details 使用 SQLite 存储会话配置信息（表名保持 group_config）
namespace insoulforge::SessionConfigManager {
    /// @brief 获取群组配置
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @return 群组配置结构体
    [[nodiscard]] SessionConfig getConfig(u64 sessionId);

    /// @brief 检查群组是否存在配置
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @return 是否存在
    [[nodiscard]] bool contains(u64 sessionId);

    /// @brief 添加群组配置
    /// @param sessionId 会话 ID（私聊会话带标志位）
    /// @param config 群组配置
    void addConfig(u64 sessionId, const SessionConfig &config = SessionConfig());

    /// @brief 增加消息计数
    /// @param sessionId 会话 ID（私聊会话带标志位）
    void incrementMessageCount(u64 sessionId);
} // namespace insoulforge::SessionConfigManager
