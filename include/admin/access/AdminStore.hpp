/// @file AdminStore.hpp
/// @brief 管理员存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：admins（QQ 管理员列表）

#pragma once

#include <infrastructure/NumericTypes.hpp>
#include <vector>


/// @brief 管理员存储
namespace insoulforge::AdminStore {
    [[nodiscard]] auto isAdmin(u64 qqNumber) -> bool;

    void addAdmin(u64 qqNumber);

    void removeAdmin(u64 qqNumber);

    [[nodiscard]] auto getAdmins() -> std::vector<u64>;
} // namespace insoulforge::AdminStore
