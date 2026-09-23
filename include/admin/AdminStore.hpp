/// @file AdminStore.hpp
/// @brief 管理员存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：admins（QQ 管理员列表）

#pragma once

#include <cstdint>
#include <infrastructure/NumericTypes.hpp>
#include <vector>

namespace insoulforge {
    /// @brief 管理员存储
    namespace AdminStore {
        [[nodiscard]] bool isAdmin(u64 qqNumber);

        void addAdmin(u64 qqNumber);

        void removeAdmin(u64 qqNumber);

        [[nodiscard]] std::vector<u64> getAdmins();
    } // namespace AdminStore
} // namespace insoulforge
