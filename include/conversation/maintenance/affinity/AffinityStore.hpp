/// @file AffinityStore.hpp
/// @brief 好感度存储
/// @details 表：group_affinity（每个会话独立维护 QQ 号 → 好感度映射，取值范围 [-100, 100]）

#pragma once

#include <unordered_map>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 好感度存储
    namespace AffinityStore {
        /// @brief 获取会话内全部用户的好感度映射
        /// @param sessionId 会话 ID
        /// @return QQ 号 → 好感度
        [[nodiscard]] auto getAffinityMap(u64 sessionId) -> std::unordered_map<u64, i32>;

        /// @brief 调整用户好感度（变化量与累计值均在 SQL 层夹紧到 [-100, 100]）
        /// @param sessionId 会话 ID
        /// @param qqNumber 用户 QQ 号
        /// @param delta 变化量
        void adjustAffinity(u64 sessionId, u64 qqNumber, i32 delta);
    } // namespace AffinityStore
} // namespace insoulforge
