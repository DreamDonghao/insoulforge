/// @file SessionStore.hpp
/// @brief 会话（群）配置与启用状态存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：group_config（消息统计）、enabled_groups（启用状态与群名称）

#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace insoulforge {
    /// @brief 会话配置结构
    struct SessionConfig {
        uint64_t allMesCount = 0; ///< 已完成主处理的入站消息总数
    };

    /// @brief 会话（群）配置与启用状态存储
    /// @details 统计数据写入 `group_config`；会话启用状态及显示名称写入 `enabled_groups`。
    ///          未存在启用状态记录的会话视为未启用。
    namespace SessionStore {
        /// @brief 获取会话的累计消息统计
        /// @param sessionId 统一会话 ID
        /// @return 已保存的统计配置；不存在时各字段均为 0
        [[nodiscard]] SessionConfig getSessionConfig(uint64_t sessionId);

        /// @brief 覆盖保存会话的累计消息统计
        /// @param sessionId 统一会话 ID
        /// @param config 待保存的消息数
        void saveSessionConfig(uint64_t sessionId, const SessionConfig &config);

        /// @brief 原子递增会话消息数
        /// @param sessionId 统一会话 ID
        /// @details 配置行不存在时自动以当前消息创建初始统计。
        void incrementMessageCount(uint64_t sessionId);

        /// @brief 检查会话是否已有统计配置
        /// @param sessionId 统一会话 ID
        /// @return 存在 `group_config` 记录时返回 true
        [[nodiscard]] bool hasSessionConfig(uint64_t sessionId);

        /// @brief 检查会话是否启用常规消息处理
        /// @param sessionId 统一会话 ID
        /// @return 启用记录存在且 `enabled` 为 true 时返回 true
        [[nodiscard]] bool isSessionEnabled(uint64_t sessionId);

        /// @brief 启用会话的常规消息处理
        /// @param sessionId 统一会话 ID
        /// @details 会重建同一会话的启用状态记录；已有显示名称会被清空。
        void enableSession(uint64_t sessionId);

        /// @brief 禁用会话的常规消息处理
        /// @param sessionId 统一会话 ID
        /// @details 删除启用状态记录；聊天记录和消息统计不会受影响。
        void disableSession(uint64_t sessionId);

        /// @brief 获取所有已启用的会话 ID
        /// @return `enabled_groups` 中 `enabled` 为 true 的会话 ID 列表
        [[nodiscard]] std::vector<uint64_t> getEnabledGroups();

        /// @brief 获取所有存在聊天记录的会话摘要
        /// @return 元组列表 `{sessionId, sessionName, recordCount}`，按记录数降序排列
        /// @details 会话未设置名称时 `sessionName` 为空字符串。
        [[nodiscard]] std::vector<std::tuple<uint64_t, std::string, int>> getSessionsWithChatRecords();

        /// @brief 获取所有已登记启用状态的会话摘要
        /// @return 元组列表 `{sessionId, sessionName, enabled, recordCount}`，优先返回已启用会话
        /// @details 仅返回 `enabled_groups` 中的记录；从未启用且没有名称的会话不在结果中。
        [[nodiscard]] std::vector<std::tuple<uint64_t, std::string, bool, int>> getAllSessionsWithStatus();

        /// @brief 切换已有会话的启用状态
        /// @param sessionId 统一会话 ID
        /// @details 不存在启用状态记录时不会创建记录。
        void toggleSessionStatus(uint64_t sessionId);

        /// @brief 更新会话显示名称
        /// @param sessionId 统一会话 ID
        /// @param name 待保存的显示名称
        /// @details 不存在启用状态记录时不会创建记录。
        void updateSessionName(uint64_t sessionId, const std::string &name);

        /// @brief 获取会话显示名称
        /// @param sessionId 统一会话 ID
        /// @return 已保存的显示名称；未设置或记录不存在时返回空字符串
        [[nodiscard]] std::string getSessionName(uint64_t sessionId);
    } // namespace SessionStore
} // namespace insoulforge
