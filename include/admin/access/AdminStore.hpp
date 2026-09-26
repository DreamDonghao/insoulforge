/// @file AdminStore.hpp
/// @brief 管理员存储
/// @details 表：admins（QQ 管理员列表）

#pragma once

#include <vector>

#include <infrastructure/NumericTypes.hpp>


/// @brief 管理员存储
namespace insoulforge::AdminStore {
    /// @brief 判断 QQ 号是否具备机器人管理命令权限
    [[nodiscard]] auto isAdmin(u64 qqNumber) -> bool;

    /// @brief 添加管理员；已存在时保持不变
    void addAdmin(u64 qqNumber);

    /// @brief 移除管理员；不存在时保持不变
    void removeAdmin(u64 qqNumber);

    /// @brief 获取全部管理员 QQ 号
    [[nodiscard]] auto getAdmins() -> std::vector<u64>;
} // namespace insoulforge::AdminStore
