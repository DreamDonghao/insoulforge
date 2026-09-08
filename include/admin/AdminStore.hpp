/// @file AdminStore.hpp
/// @brief 管理员存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：admins（QQ 管理员列表）

#pragma once
#include <cstdint>
#include <vector>

namespace insoulforge {
    /// @brief 管理员存储
    namespace AdminStore {
        [[nodiscard]] bool isAdmin(uint64_t qqNumber);

        void addAdmin(uint64_t qqNumber);

        void removeAdmin(uint64_t qqNumber);

        [[nodiscard]] std::vector<uint64_t> getAdmins();
    } // namespace AdminStore
} // namespace insoulforge
