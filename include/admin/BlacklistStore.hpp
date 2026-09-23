/// @file BlacklistStore.hpp
/// @brief 全局 QQ 黑名单持久化接口

#pragma once

#include <infrastructure/NumericTypes.hpp>

#include <cstdint>
#include <vector>

namespace insoulforge::BlacklistStore {
    /// @brief 判断 QQ 是否在全局黑名单中。
    [[nodiscard]] bool contains(u64 qqNumber);

    /// @brief 将 QQ 加入全局黑名单；已存在时保持不变。
    void add(u64 qqNumber);

    /// @brief 将 QQ 移出全局黑名单；不存在时保持不变。
    void remove(u64 qqNumber);

    /// @brief 获取全部黑名单 QQ 号。
    [[nodiscard]] std::vector<u64> getAll();
} // namespace insoulforge::BlacklistStore
