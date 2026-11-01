/// @file AdminStore.hpp
/// @brief 管理员存储
/// @details 表：admins（用户管理员列表）

#pragma once

#include <vector>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>

/// @brief 管理员存储
namespace insoulforge::AdminStore {
    /// @brief 判断用户 ID 是否具备机器人管理命令权限
    [[nodiscard]] auto isAdmin(const UserId &userId) -> bool;

    /// @brief 添加管理员；已存在时保持不变
    void addAdmin(const UserId &userId);

    /// @brief 移除管理员；不存在时保持不变
    void removeAdmin(const UserId &userId);

    /// @brief 获取全部管理员 用户 ID
    [[nodiscard]] auto getAdmins() -> std::vector<UserId>;
} // namespace insoulforge::AdminStore
