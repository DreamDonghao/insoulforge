/// @file AffinityStore.hpp
/// @brief 好感度存储
/// @details 表：person_affinity（全局 QQ 号 → 好感度映射，取值范围 [-100, 100]）

#pragma once

#include <unordered_map>

#include <infrastructure/NumericTypes.hpp>


/// @brief 好感度存储
namespace insoulforge::AffinityStore {
    /// @brief 获取全局用户的好感度映射
    /// @return QQ 号 → 好感度
    [[nodiscard]] auto getAffinityMap() -> std::unordered_map<u64, i32>;

    /// @brief 调整用户好感度（变化量与累计值均在 SQL 层夹紧到 [-100, 100]）
    /// @param qqNumber 用户 QQ 号
    /// @param delta 变化量
    void adjustAffinity(u64 qqNumber, i32 delta);
} // namespace insoulforge::AffinityStore
