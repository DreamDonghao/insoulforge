/// @file SessionStore.hpp
/// @brief 群聊与私聊的统计、启用状态和名称存储
/// @details 表：group_config（消息统计）、enabled_groups（启用状态与群名称）

#pragma once

#include <string>
#include <tuple>
#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 会话配置结构
    struct SessionConfig {
        u64 allMesCount = 0; ///< 已完成主处理的入站消息总数
    };

    /// @brief 会话统计与启用状态的数据库接口
    /// @details 统计数据写入 `group_config`；会话启用状态及显示名称写入 `enabled_groups`。
    ///          未存在启用状态记录的会话视为未启用。
    namespace SessionStore {
        /// @brief 获取会话的累计消息统计
        /// @param sessionId 统一会话 ID
        /// @return 已保存的统计配置；不存在时各字段均为 0
        [[nodiscard]] auto getSessionConfig(u64 sessionId) -> SessionConfig;

        /// @brief 覆盖保存会话的累计消息统计
        /// @param sessionId 统一会话 ID
        /// @param config 待保存的消息数
        void saveSessionConfig(u64 sessionId, const SessionConfig &config);

        /// @brief 原子递增会话消息数
        /// @param sessionId 统一会话 ID
        /// @details 配置行不存在时自动以当前消息创建初始统计。
        void incrementMessageCount(u64 sessionId);

        /// @brief 检查会话是否已有统计配置
        /// @param sessionId 统一会话 ID
        /// @return 存在 `group_config` 记录时返回 true
        [[nodiscard]] auto hasSessionConfig(u64 sessionId) -> bool;

        /// @brief 检查会话是否启用常规消息处理
        /// @param sessionId 统一会话 ID
        /// @return 启用记录存在且 `enabled` 为 true 时返回 true
        [[nodiscard]] auto isSessionEnabled(u64 sessionId) -> bool;

        /// @brief 启用会话的常规消息处理
        /// @param sessionId 统一会话 ID
        /// @details 会重建同一会话的启用状态记录；已有显示名称会被清空。
        void enableSession(u64 sessionId);

        /// @brief 禁用会话的常规消息处理
        /// @param sessionId 统一会话 ID
        /// @details 删除启用状态记录；聊天记录和消息统计不会受影响。
        void disableSession(u64 sessionId);

        /// @brief 获取所有已启用的会话 ID
        /// @return `enabled_groups` 中 `enabled` 为 true 的会话 ID 列表
        [[nodiscard]] auto getEnabledGroups() -> std::vector<u64>;

        /// @brief 获取所有存在聊天记录的会话摘要
        /// @return 元组列表 `{sessionId, sessionName, recordCount}`，按记录数降序排列
        /// @details 会话未设置名称时 `sessionName` 为空字符串。
        [[nodiscard]] auto getSessionsWithChatRecords() -> std::vector<std::tuple<u64, std::string, i32>>;

        /// @brief 获取所有已登记启用状态的会话摘要
        /// @return 元组列表 `{sessionId, sessionName, enabled, recordCount}`，优先返回已启用会话
        /// @details 仅返回 `enabled_groups` 中的记录；从未启用且没有名称的会话不在结果中。
        [[nodiscard]] auto getAllSessionsWithStatus() -> std::vector<std::tuple<u64, std::string, bool, i32>>;

        /// @brief 切换已有会话的启用状态
        /// @param sessionId 统一会话 ID
        /// @details 不存在启用状态记录时不会创建记录。
        void toggleSessionStatus(u64 sessionId);

        /// @brief 更新会话显示名称
        /// @param sessionId 统一会话 ID
        /// @param name 待保存的显示名称
        /// @details 不存在启用状态记录时不会创建记录。
        void updateSessionName(u64 sessionId, const std::string &name);

        /// @brief 获取会话显示名称
        /// @param sessionId 统一会话 ID
        /// @return 已保存的显示名称；未设置或记录不存在时返回空字符串
        [[nodiscard]] auto getSessionName(u64 sessionId) -> std::string;
    } // namespace SessionStore
} // namespace insoulforge
