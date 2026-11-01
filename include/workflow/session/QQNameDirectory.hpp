/// @file QQNameDirectory.hpp
/// @brief 运行时用户昵称目录

#pragma once

#include <string>
#include <unordered_map>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge::QQNameDirectory {
    /// @brief 记录 OneBot 上报中观察到的用户昵称
    /// @details 已配置的自定义昵称优先级更高，不会被上报昵称覆盖。
    /// @param userId 用户 ID
    /// @param name 上报中的昵称
    void recordName(const UserId &userId, std::string name);

    /// @brief 设置用户 ID 的自定义昵称
    /// @param userId 用户 ID
    /// @param name 自定义昵称
    void setCustomName(const UserId &userId, std::string name);

    /// @brief 查询用户 ID 的当前昵称
    /// @param userId 用户 ID
    /// @return 已知昵称；未记录时返回“未知”
    [[nodiscard]] auto getName(const UserId &userId) -> std::string;

    /// @brief 获取昵称到用户 ID 的反向索引快照
    /// @return 独立副本，调用者可安全遍历或修改
    [[nodiscard]] auto nameToQQMap() -> std::unordered_map<std::string, UserId>;
} // namespace insoulforge::QQNameDirectory
