/// @file AffinityStore.hpp
/// @brief 好感度存储
/// @author donghao
/// @date 2026-08-31
/// @details 表：group_affinity（每个会话独立维护 QQ 号 → 好感度映射，取值范围 [-100, 100]）

#pragma once
#include <cstdint>
#include <unordered_map>

namespace insoulforge {
    /// @brief 好感度存储
    namespace AffinityStore {
        /// @brief 获取会话内全部用户的好感度映射
        /// @param sessionId 会话 ID
        /// @return QQ 号 → 好感度
        [[nodiscard]] std::unordered_map<uint64_t, int> getAffinityMap(uint64_t sessionId);

        /// @brief 调整用户好感度（变化量与累计值均在 SQL 层夹紧到 [-100, 100]）
        /// @param sessionId 会话 ID
        /// @param qqNumber 用户 QQ 号
        /// @param delta 变化量
        void adjustAffinity(uint64_t sessionId, uint64_t qqNumber, int delta);
    } // namespace AffinityStore
} // namespace insoulforge
