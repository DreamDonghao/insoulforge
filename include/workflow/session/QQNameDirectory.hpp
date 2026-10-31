/// @file QQNameDirectory.hpp
/// @brief 运行时 QQ 昵称目录

#pragma once

#include <string>
#include <unordered_map>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::QQNameDirectory {
    /// @brief 记录 OneBot 上报中观察到的 QQ 昵称
    /// @details 已配置的自定义昵称优先级更高，不会被上报昵称覆盖。
    /// @param qqNumber 用户 ID
    /// @param name 上报中的昵称
    void recordName(u64 qqNumber, std::string name);

    /// @brief 设置 用户 ID的自定义昵称
    /// @param qqNumber 用户 ID
    /// @param name 自定义昵称
    void setCustomName(u64 qqNumber, std::string name);

    /// @brief 查询 用户 ID的当前昵称
    /// @param qqNumber 用户 ID
    /// @return 已知昵称；未记录时返回“未知”
    [[nodiscard]] auto getName(u64 qqNumber) -> std::string;

    /// @brief 获取昵称到 用户 ID的反向索引快照
    /// @return 独立副本，调用者可安全遍历或修改
    [[nodiscard]] auto nameToQQMap() -> std::unordered_map<std::string, u64>;
} // namespace insoulforge::QQNameDirectory
