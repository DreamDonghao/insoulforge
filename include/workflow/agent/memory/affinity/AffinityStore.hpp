/// @file AffinityStore.hpp
/// @brief 好感度存储
/// @details 表：person_affinity（全局 用户 ID → 好感度映射，取值范围 [-100, 100]）

#pragma once

#include <unordered_map>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>


/// @brief 好感度存储
namespace insoulforge::AffinityStore {
    /// @brief 获取全局用户的好感度映射
    /// @return 用户 ID → 好感度
    [[nodiscard]] auto getAffinityMap() -> std::unordered_map<UserId, i32>;

    /// @brief 调整用户好感度（变化量与累计值均在 SQL 层夹紧到 [-100, 100]）
    /// @param userId 用户 用户 ID
    /// @param delta 变化量
    void adjustAffinity(const UserId &userId, i32 delta);
} // namespace insoulforge::AffinityStore
